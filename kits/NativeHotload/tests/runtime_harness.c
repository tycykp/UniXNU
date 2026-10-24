/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Executes the real runtime with host OS/VM shims and native test callbacks.
 * The memory shim maps four-byte test image IDs to host callbacks: this does
 * NOT execute ARM64 image bytes, emulate pmap or exercise physical hardware.
 */
#include "shim.h"
#include NATIVE_RUNTIME_SOURCE

static const char *current_case;
static void begin(const char *name) { current_case = name; printf("@BEGIN %s\n", name); fflush(stdout); }
static void finish(void) { printf("@RESULT {\"id\":\"%s\",\"status\":\"pass\"}\n", current_case); fflush(stdout); }
static void failed(const char *condition, int line)
{
    printf("assertion line=%d expression=%s\n", line, condition);
    printf("@RESULT {\"id\":\"%s\",\"status\":\"fail\",\"line\":%d}\n", current_case, line);
    exit(1);
}
#define CHECK(x) do { if (!(x)) failed(#x, __LINE__); } while (0)

static unsigned created, destroyed, provider_count, denied, lifecycle_denied;
static struct nlh_invocation observed[20000];
static unsigned held, release_held;
static _Thread_local int (*saved_platform)(void *, uint32_t, uint64_t, uint64_t, uint64_t *);
static _Thread_local void *saved_context;
static unsigned nested_denied;
static unsigned concurrent_successes, concurrent_busy;
struct test_state { uint64_t count; unsigned running; };

static int provider(const struct nlh_invocation *ctx, uint32_t op,
                    uint64_t a, uint64_t b, uint64_t *out)
{
    unsigned index = __atomic_fetch_add(&provider_count, 1, __ATOMIC_RELAXED);
    CHECK(index < sizeof(observed) / sizeof(observed[0]));
    observed[index] = *ctx;
    CHECK(ctx->abi == NLH_PLATFORM_ABI && ctx->bytes == sizeof(*ctx));
    CHECK(ctx->phase == b && ctx->argument == a && ctx->image_cookie && ctx->invocation_id);
    if (op == 2) {
        uint64_t ignored;
        CHECK(nlh_dispatch_resident("harness", 1, 0, &ignored) == EACCES);
        __atomic_fetch_add(&nested_denied, 1, __ATOMIC_RELAXED);
    }
    if (ctx->origin != NLH_ORIGIN_RESIDENT || ctx->phase != NLH_CALL ||
        ctx->role != NLH_ROLE_ACTIVE || ctx->depth) {
        __atomic_fetch_add(&denied, 1, __ATOMIC_RELAXED);
        return EACCES;
    }
    *out = ctx->invocation_id;
    return 0;
}

static int entry(unsigned variant, const struct nlh_host *host, void *opaque,
                 uint32_t op, uint64_t arg, uint64_t *result)
{
    struct test_state *state = opaque;
    uint64_t ignored = 0;
    int access = host->platform(host->context, 1, arg, op, &ignored);
    if (op == NLH_PROBE || op == NLH_EXPORT || op == NLH_IMPORT) {
        CHECK(access == EACCES);
        CHECK(host->call(host->context, 0, 0, &ignored) == EACCES);
        __atomic_fetch_add(&lifecycle_denied, 1, __ATOMIC_RELAXED);
    }
    CHECK(host->platform((void *)(uintptr_t)1, 1, arg, op, &ignored) == EACCES);
    CHECK(nlh_dispatch_resident("harness", 1, 0, &ignored) == EACCES);
    saved_platform = host->platform; saved_context = host->context;
    switch (op) {
    case NLH_PROBE: return 0;
    case NLH_START:
        state->running = 1;
        return variant == 3 ? EIO : 0;
    case NLH_STOP:
        if (variant == 5) return EIO;
        state->running = 0; return 0;
    case NLH_EXPORT: {
        struct nlh_transfer *t = (void *)(uintptr_t)arg;
        CHECK(t->capacity >= sizeof(state->count));
        memcpy(t->data, &state->count, sizeof(state->count)); t->bytes = sizeof(state->count); return 0;
    }
    case NLH_IMPORT: {
        if (variant == 4) return EIO;
        struct nlh_transfer *t = (void *)(uintptr_t)arg;
        CHECK(t->bytes == sizeof(state->count));
        memcpy(&state->count, t->data, sizeof(state->count)); return 0;
    }
    case NLH_CALL:
        CHECK(state->running);
        if (arg == 42) CHECK(!host->platform(host->context,2,arg,op,&ignored));
        if (variant == 6) return host->call(host->context, 0, arg, result);
        if (arg == UINT64_MAX) {
            __atomic_store_n(&held, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&release_held, __ATOMIC_ACQUIRE)) usleep(1000);
            arg = 0;
        }
        *result = __atomic_add_fetch(&state->count, arg + 1, __ATOMIC_RELAXED);
        return 0;
    default: return EINVAL;
    }
}
#define ENTRY(n) static int entry##n(const struct nlh_host *h, void *s, uint32_t o, uint64_t a, uint64_t *r) { return entry(n,h,s,o,a,r); }
ENTRY(1) ENTRY(2) ENTRY(3) ENTRY(4) ENTRY(5) ENTRY(6)
static nlh_entry_fn memory_entries[NLH_MAX_MODULES * 2];
static nlh_entry_fn test_memory_override;
static uint32_t test_interface = 1;
const char nlh_memory_build[] = "NLH host-shim (not ARM64 or pmap validation)";
int nlh_memory_enabled(void) { return 1; }
void nlh_memory_status(uint32_t nonce) { printf("NLH memory nonce=%u snapshot=1 arena=1 error=0\n", nonce); }
int nlh_memory_create(unsigned slot, const void *code, unsigned length)
{
    nlh_entry_fn entries[] = {NULL, entry1, entry2, entry3, entry4, entry5, entry6};
    unsigned id = *(const unsigned char *)code;
    CHECK(length == 4 && slot < NLH_MAX_MODULES * 2 && id && id < 7 && !memory_entries[slot]);
    memory_entries[slot] = test_memory_override ? test_memory_override : entries[id]; created++; return 0;
}
void *nlh_memory_address(unsigned slot) { return (void *)(uintptr_t)memory_entries[slot]; }
void nlh_memory_destroy(unsigned slot) { CHECK(memory_entries[slot]); memory_entries[slot] = NULL; destroyed++; }

static void pump(void)
{
    CHECK(test_worker);
    void (*worker)(void *, wait_result_t) = test_worker;
    test_worker = NULL;
    worker(test_worker_arg, 0);
}
static void upload(const char *name, unsigned version, unsigned variant, const char *dependency, int run)
{
    struct nlh_header h = { .magic=NLH_MAGIC, .abi=NLH_ABI, .header_bytes=sizeof(h),
        .code_bytes=4, .state_bytes=sizeof(struct test_state), .version=version,
        .interface_abi=test_interface, .state_abi=1, .kind=NLH_SERVICE };
    strcpy(h.name, name);
    if (dependency) { h.dependency_count=1; strcpy(h.dependencies[0].name,dependency); h.dependencies[0].interface_abi=1; h.dependencies[0].minimum_version=1; }
    unsigned char data[sizeof(h)+4], code[4] = {(unsigned char)variant,0,0,0};
    SHA256_CTX hash; SHA256_Init(&hash); SHA256_Update(&hash,&h,sizeof(h)-32);
    SHA256_Update(&hash,code,4); SHA256_Final(h.sha256,&hash);
    memcpy(data,&h,sizeof(h)); memcpy(data+sizeof(h),code,4);
    char command[64]; snprintf(command,sizeof(command),"hl.begin %zu",sizeof(data));
    CHECK(nlh_command(command)); CHECK(nlh.uploading && !nlh.busy);
    for (unsigned offset=0; offset<sizeof(data); offset+=16) {
        unsigned size=sizeof(data)-offset; if (size>16) size=16;
        int prefix=snprintf(command,sizeof(command),"hl.data %u %u ",nlh.token,offset);
        for (unsigned j=0;j<size;j++) snprintf(command+prefix+j*2,3,"%02x",data[offset+j]);
        CHECK(strlen(command)<=63 && nlh_command(command)); CHECK(nlh.received==offset+size);
    }
    snprintf(command,sizeof(command),"hl.commit %u",nlh.token);
    CHECK(nlh_command(command)); CHECK(nlh.busy);
    if (run) pump();
}
static void unload(const char *name)
{ char cmd[64]; snprintf(cmd,sizeof(cmd),"hl.unload %s",name); CHECK(nlh_command(cmd)); CHECK(nlh.busy); pump(); }
static void *call_one(void *unused)
{ (void)unused; uint64_t out; CHECK(!nlh_dispatch_resident("harness",1,UINT64_MAX,&out)); return NULL; }
static void *update_one(void *unused) { (void)unused; pump(); return NULL; }
static void *many_calls(void *arg)
{
    for (unsigned j=0;j<100;j++) {
        uint64_t out;
        int error=nlh_dispatch_resident("harness",1,(uintptr_t)arg,&out);
        CHECK(!error || error==EBUSY);
        __atomic_fetch_add(error ? &concurrent_busy : &concurrent_successes,1,__ATOMIC_RELAXED);
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    begin("registration-before-upload");
    CHECK(nlh_register_platform(NLH_ABI,NULL)==ENOTSUP);
    CHECK(nlh_register_platform_v2(NLH_PLATFORM_ABI,provider)==0);
    CHECK(nlh_register_platform_v2(NLH_PLATFORM_ABI,provider)==EBUSY); finish();

    begin("upload-real-entry-and-origin-authentication");
    upload("harness",1,1,NULL,1); CHECK(!nlh.error && nlh_slots[0].state==NLH_RUNNING);
    CHECK(created==1 && lifecycle_denied);
    uint64_t out=0; unsigned p=provider_count;
    CHECK(!nlh_dispatch_resident("harness",1,2,&out) && out==3);
    CHECK(observed[p].phase==NLH_CALL && observed[p].origin==NLH_ORIGIN_RESIDENT && observed[p].role==NLH_ROLE_ACTIVE);
    unsigned rejected=denied;
    CHECK(!nlh_dispatch("harness",1,0,&out) && denied>rejected);
    CHECK(nlh_command("hl.call harness 0")); pump();
    CHECK(observed[provider_count-1].origin==NLH_ORIGIN_USB);
    CHECK(saved_platform(saved_context,1,0,NLH_CALL,&out)==EACCES);
    CHECK(!nlh_dispatch_resident("harness",1,42,&out) && nested_denied);
    CHECK(nlh_register_platform_v2(NLH_PLATFORM_ABI,provider)==EBUSY); finish();

    begin("replacement-and-private-state-migration");
    uint64_t count=((struct test_state *)nlh_slots[0].images[nlh_slots[0].active].state)->count;
    uint64_t cookie=nlh_slots[0].images[nlh_slots[0].active].cookie;
    unsigned generation=nlh_slots[0].generation;
    upload("harness",2,2,NULL,1); CHECK(!nlh.error && nlh_slots[0].generation>generation);
    CHECK(nlh_slots[0].images[nlh_slots[0].active].cookie!=cookie);
    CHECK(!nlh_dispatch_resident("harness",1,0,&out) && out==count+1); finish();

    begin("start-failure-cleanup-and-rollback");
    uint64_t epoch=nlh_slots[0].images[nlh_slots[0].active].activation_epoch;
    upload("harness",3,3,NULL,1); CHECK(nlh.error==EIO && !nlh.recovery_error);
    CHECK(nlh_slots[0].state==NLH_RUNNING && nlh_slots[0].images[nlh_slots[0].active].header.version==2);
    CHECK(nlh_slots[0].images[nlh_slots[0].active].activation_epoch>epoch);
    CHECK(!nlh_dispatch_resident("harness",1,0,&out)); finish();

    begin("import-failure-and-rollback");
    upload("harness",4,4,NULL,1); CHECK(nlh.error==EIO && !nlh.recovery_error && nlh_slots[0].state==NLH_RUNNING);
    CHECK(!nlh_dispatch_resident("harness",1,0,&out)); finish();

    begin("concurrent-invocations-isolated-identities");
    unsigned first=provider_count; pthread_t threads[8];
    for(unsigned i=0;i<8;i++) CHECK(!pthread_create(&threads[i],NULL,many_calls,(void *)(uintptr_t)(i+1)));
    for(unsigned i=0;i<8;i++) CHECK(!pthread_join(threads[i],NULL));
    CHECK(concurrent_successes && concurrent_successes+concurrent_busy==800);
    CHECK(provider_count-first==concurrent_successes);
    for(unsigned i=first;i<provider_count;i++) for(unsigned j=first;j<i;j++)
        CHECK(observed[i].invocation_id!=observed[j].invocation_id);
    CHECK(!nlh_frames); finish();

    begin("drain-close-concurrent-old-call");
    pthread_t caller,updater;
    CHECK(!pthread_create(&caller,NULL,call_one,NULL));
    for(unsigned i=0;i<2000 && !__atomic_load_n(&held,__ATOMIC_ACQUIRE);i++) usleep(1000);
    CHECK(held);
    upload("harness",5,2,NULL,0); CHECK(!pthread_create(&updater,NULL,update_one,NULL));
    for(unsigned i=0;i<2000 && !(__atomic_load_n(&nlh_slots[0].admission,__ATOMIC_ACQUIRE)&NLH_CLOSED);i++) usleep(1000);
    CHECK(nlh_dispatch_resident("harness",1,0,&out)==EBUSY);
    __atomic_store_n(&release_held,1,__ATOMIC_RELEASE);
    CHECK(!pthread_join(caller,NULL) && !pthread_join(updater,NULL));
    CHECK(!nlh.error && nlh_slots[0].images[nlh_slots[0].active].header.version==5); finish();

    begin("dependency-cannot-inherit-resident-authority");
    upload("consumer",1,6,"harness",1); CHECK(!nlh.error);
    rejected=denied; p=provider_count;
    CHECK(!nlh_dispatch_resident("consumer",1,0,&out));
    CHECK(denied>rejected && observed[provider_count-1].origin==NLH_ORIGIN_DEPENDENCY);
    CHECK(observed[provider_count-1].root_origin==NLH_ORIGIN_RESIDENT && observed[provider_count-1].depth==1);
    unload("consumer"); CHECK(!nlh.error); finish();

    begin("unload-and-resident-fallback");
    unload("harness"); CHECK(!nlh.error && nlh_slots[0].state==NLH_EMPTY);
    unsigned calls_before=provider_count, fallbacks=0;
    uint64_t baseline=77;
    for(unsigned i=0;i<10;i++) { out=baseline; if(nlh_dispatch_resident("harness",1,0,&out)) { out=baseline; fallbacks++; } CHECK(out==baseline); }
    CHECK(fallbacks==10 && calls_before==provider_count && created==destroyed); finish();

    /* Destructive uncertainty is LAST. Retain code/state and do not retry. */
    begin("stop-failure-quarantine-last");
    upload("harness",6,5,NULL,1); CHECK(!nlh.error);
    unsigned retained=created-destroyed;
    unload("harness"); CHECK(nlh.error==EIO && nlh_slots[0].state==NLH_QUARANTINED);
    CHECK(created-destroyed==retained && (nlh_slots[0].admission&NLH_CLOSED));
    CHECK(nlh_dispatch_resident("harness",1,0,&out)==EBUSY);
    CHECK(!nlh_frames); finish();
    printf("@COMPLETE retained_images=%u quarantine_requires_process_exit=1\n",created-destroyed);
    return 0;
}
