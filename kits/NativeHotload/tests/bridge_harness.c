/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Same actual NLH runtime + actual giulia bridge, with host memory/OS shims.
 * Native callbacks below are test fixtures, never claimed as ARM64/hardware.
 */
#define main runtime_fixture_main
#include "runtime_harness.c"
#undef main
#include HOTLOAD_BRIDGE_SOURCE

static unsigned bridge_behavior, bridge_entered, bridge_release;
static _Thread_local uint64_t bridge_last_ticket;
static uint64_t bridge_nested_epoch;
static int protocol_entry(const struct nlh_host *host, void *opaque,
    uint32_t op, uint64_t argument, uint64_t *result)
{
    struct test_state *state = opaque;
    uint64_t ignored;
    if(op != NLH_CALL) {
        CHECK(host->platform(host->context, GH_PLATFORM_GET_META, argument,
              GH_META_DOMAIN,&ignored)==EACCES);
        switch(op) {
        case NLH_PROBE: return 0;
        case NLH_START: state->running=1; return 0;
        case NLH_STOP: state->running=0; return 0;
        case NLH_EXPORT: {
            struct nlh_transfer *t=(void *)(uintptr_t)argument;
            memcpy(t->data,&state->count,sizeof(state->count)); t->bytes=sizeof(state->count); return 0;
        }
        case NLH_IMPORT: {
            struct nlh_transfer *t=(void *)(uintptr_t)argument;
            CHECK(t->bytes==sizeof(state->count)); memcpy(&state->count,t->data,sizeof(state->count)); return 0;
        }
        default: return EINVAL;
        }
    }
    CHECK(state->running);
    bridge_last_ticket=argument;
    uint64_t requests=0,responses=0,domain=0,epoch=0;
    int error=host->platform(host->context,GH_PLATFORM_GET_META,argument,GH_META_DOMAIN,&domain);
    if(error) return error;
    CHECK(!host->platform(host->context,GH_PLATFORM_GET_META,argument,GH_META_EPOCH,&epoch));
    CHECK(!host->platform(host->context,GH_PLATFORM_GET_META,argument,GH_META_REQUEST_WORDS,&requests));
    CHECK(!host->platform(host->context,GH_PLATFORM_GET_META,argument,GH_META_RESPONSE_WORDS,&responses));
    if(bridge_behavior==6) {
        uint64_t request=3,response=99;
        CHECK(giulia_hotload_select(domain,bridge_nested_epoch,1,&request,1,&response,1)==EBUSY && response==99);
    }
    if(bridge_behavior==7) {
        __atomic_fetch_add(&bridge_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&bridge_release,__ATOMIC_ACQUIRE)) usleep(1000);
    }
    for(unsigned i=0;i<responses;i++) {
        uint64_t value=0;
        if(bridge_behavior!=2)
            CHECK(!host->platform(host->context,GH_PLATFORM_READ_REQUEST_BASE+i%requests,argument,0,&value));
        if(bridge_behavior!=1 || i+1<responses)
            CHECK(!host->platform(host->context,GH_PLATFORM_WRITE_RESPONSE_BASE+i,argument,value+1,&ignored));
    }
    if(bridge_behavior==3)
        CHECK(host->platform(host->context,GH_PLATFORM_WRITE_RESPONSE_BASE+responses,argument,1,&ignored)==EINVAL);
    if(bridge_behavior==5)
        CHECK(host->platform(host->context,GH_PLATFORM_GET_META,argument-1,0,&ignored)==ESTALE);
    __atomic_fetch_add(&state->count,1,__ATOMIC_RELAXED);
    *result=0;
    return bridge_behavior==4 ? EIO : 0;
}

static void *bridge_thread(void *opaque)
{
    uint32_t domain=(uintptr_t)opaque;
    uint64_t request=domain,response=77;
    CHECK(!giulia_hotload_select(domain,20,1,&request,1,&response,1));
    CHECK(response==domain+1);
    return NULL;
}

int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    begin("bridge-source-identity-registration");
    if(!gh_source_valid()) {
        CHECK(giulia_hotload_init()==EINVAL && giulia_hotload_init()==EINVAL);
        CHECK(!nlh_platform_call);
        finish(); return 0;
    }
    CHECK(!giulia_hotload_init() && !giulia_hotload_init());
    CHECK(nlh_platform_call==gh_platform); finish();
    test_memory_override=protocol_entry; test_interface=GH_INTERFACE_ABI;

    begin("bridge-missing-module-preserves-baseline");
    uint64_t request[GH_MAX_WORDS],response[GH_MAX_WORDS];
    for(unsigned i=0;i<GH_MAX_WORDS;i++) { request[i]=i+10; response[i]=77; }
    CHECK(giulia_hotload_select(GH_DOMAIN_GPU,1,1,request,1,response,1)==ENOENT && response[0]==77);
    CHECK(gh_mailboxes[0].stats.fallbacks==1); finish();

    begin("bridge-real-resident-request-response");
    upload(GH_NAME_GPU,1,1,NULL,1); CHECK(!nlh.error);
    CHECK(!giulia_hotload_select(GH_DOMAIN_GPU,2,1,request,1,response,1));
    CHECK(response[0]==11 && gh_mailboxes[0].stats.calls==1 && gh_mailboxes[0].stats.executed==1);
    CHECK(gh_mailboxes[0].ticket==0 && !gh_mailboxes[0].bound);
    CHECK(gh_mailboxes[0].stats.image_cookie && gh_mailboxes[0].stats.invocation_id);
    uint64_t expired_ticket=bridge_last_ticket; finish();

    begin("bridge-one-thirtytwo-word-boundaries");
    CHECK(!giulia_hotload_select(GH_DOMAIN_GPU,3,1,request,GH_MAX_WORDS,response,GH_MAX_WORDS));
    for(unsigned i=0;i<GH_MAX_WORDS;i++) CHECK(response[i]==request[i]+1);
    response[0]=77;
    CHECK(giulia_hotload_select(GH_DOMAIN_GPU,3,1,request,0,response,1)==EINVAL && response[0]==77);
    CHECK(giulia_hotload_select(GH_DOMAIN_GPU,3,1,request,1,response,33)==EINVAL && response[0]==77); finish();

    begin("bridge-usb-generic-and-expired-ticket-rejected");
    uint64_t out=0;
    CHECK(nlh_dispatch(GH_NAME_GPU,GH_INTERFACE_ABI,expired_ticket,&out)==EACCES);
    char command[64]; snprintf(command,sizeof(command),"hl.call %s %llu",GH_NAME_GPU,(unsigned long long)expired_ticket);
    CHECK(nlh_command(command)); pump(); CHECK(nlh.error==EACCES);
    CHECK(nlh_dispatch_resident(GH_NAME_GPU,GH_INTERFACE_ABI,expired_ticket,&out)==ESTALE);
    CHECK(gh_mailboxes[0].ticket==0 && response[0]==77); finish();

    begin("bridge-malformed-or-failed-response-preserves-baseline");
    for(unsigned behavior=1;behavior<=5;behavior++) {
        bridge_behavior=behavior;
        for(unsigned i=0;i<GH_MAX_WORDS;i++) response[i]=77;
        int error=giulia_hotload_select(GH_DOMAIN_GPU,4,1,request,2,response,2);
        CHECK(error==(behavior==3 ? EINVAL : behavior==4 ? EIO : behavior==5 ? ESTALE : EPROTO));
        for(unsigned i=0;i<GH_MAX_WORDS;i++) CHECK(response[i]==77);
    }
    bridge_behavior=0; finish();

    begin("bridge-old-epoch-after-replacement-rejected");
    CHECK(!giulia_hotload_select(GH_DOMAIN_GPU,10,1,request,1,response,1));
    uint64_t calls=gh_mailboxes[0].stats.calls;
    upload(GH_NAME_GPU,2,2,NULL,1); CHECK(!nlh.error);
    response[0]=77;
    CHECK(giulia_hotload_select(GH_DOMAIN_GPU,9,1,request,1,response,1)==ESTALE && response[0]==77);
    CHECK(gh_mailboxes[0].stats.calls==calls && gh_mailboxes[0].high_epoch==10);
    CHECK(!giulia_hotload_select(GH_DOMAIN_GPU,10,1,request,1,response,1));
    CHECK(gh_mailboxes[0].stats.image_version==2); finish();

    begin("bridge-same-domain-no-wait-reentrance");
    bridge_behavior=6; bridge_nested_epoch=10;
    CHECK(!giulia_hotload_select(GH_DOMAIN_GPU,10,1,request,1,response,1));
    CHECK(gh_mailboxes[0].stats.busy);
    bridge_behavior=0; finish();

    begin("bridge-cross-domain-concurrent-mailboxes");
    upload(GH_NAME_WIFI,1,1,NULL,1); CHECK(!nlh.error);
    bridge_behavior=7;
    pthread_t gpu,wifi;
    CHECK(!pthread_create(&gpu,NULL,bridge_thread,(void *)(uintptr_t)GH_DOMAIN_GPU));
    CHECK(!pthread_create(&wifi,NULL,bridge_thread,(void *)(uintptr_t)GH_DOMAIN_WIFI));
    for(unsigned i=0;i<2000 && __atomic_load_n(&bridge_entered,__ATOMIC_ACQUIRE)!=2;i++) usleep(1000);
    CHECK(bridge_entered==2);
    __atomic_store_n(&bridge_release,1,__ATOMIC_RELEASE);
    CHECK(!pthread_join(gpu,NULL) && !pthread_join(wifi,NULL));
    bridge_behavior=0; CHECK(!nlh_frames); finish();

    begin("bridge-unload-resident-baseline-fallback");
    unload(GH_NAME_GPU); CHECK(!nlh.error);
    calls=gh_mailboxes[0].stats.calls; response[0]=77;
    CHECK(giulia_hotload_select(GH_DOMAIN_GPU,21,1,request,1,response,1)==EBUSY && response[0]==77);
    CHECK(gh_mailboxes[0].stats.calls==calls);
    unload(GH_NAME_WIFI); CHECK(!nlh.error && created==destroyed);
    giulia_hotload_status(); finish();
    printf("@COMPLETE actual_runtime_and_bridge=1 host_shims=1 hardware_execution=0\n");
    return 0;
}
