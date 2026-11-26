// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PicassoRecord.h"
#include "PicassoDataInit.h"
// Board-private records in the existing 1 GiB reserve, disjoint from Data/aux.
// NVRAM: first six 4K blocks of the otherwise-unused export region.
// Console: two 260 KiB slots at the beginning of the final 32 MiB log region.
// Legacy raw log/export APIs stay disabled (sheng-log is absent).
namespace {
struct PicassoRecordOps {
    bool read(uint64_t b,uint32_t n,void *p) {
        while(n){uint32_t k=n>32?32:n;if(hostRead4k(0,b,k,p)!=kIOReturnSuccess)return false;
            b+=k;n-=k;p=static_cast<uint8_t *>(p)+k*4096;}return true;
    }
    bool write(uint64_t b,uint32_t n,const void *p) {
        while(n){uint32_t k=n>32?32:n;IOReturn status=hostWrite4k(0,b,k,p,true);
            /* Round 13: a refused raw write used to be invisible -- the record simply
             * never appeared and nothing said why. Name the failing window here. */
            if(status!=kIOReturnSuccess){IOLog("PICASSO_RECORD write refused lba=%llu blocks=%u status=%08x\n",
                (unsigned long long)b,k,(unsigned)status);return false;}
            b+=k;n-=k;p=static_cast<const uint8_t *>(p)+k*4096;}return true;
    }
    bool sync() {return hostSyncCache()==kIOReturnSuccess;}
};
IOLock *gPicassoRecordLock;
picasso_record::Record gPicassoNvram, gPicassoLog;
uint8_t gPicassoNvramImage[8192],gPicassoNvramScratch[8192];
bool gPicassoNvramLoaded;
// Called serially by the pre-publication Data worker, then records startup.
bool gPicassoRecordWindowOpened;
static bool picassoRecordWindowOpen() {
    if(gPicassoRecordWindowOpened)return true;
    uint64_t partition=gStoreGeometry.last-gStoreGeometry.first+1;
    if(storeMode()!=2 || logMode()!=0 || !gStoreGeometry.valid ||
       reserveBlocks()<kLogReserveBlocks+kExportBlocks || reserveBlocks()>=partition ||
       partition-reserveBlocks()<(gStoreGeometry.imageBytes+4095)/4096+1024) {
        IOLog("PICASSO_DATA_INIT record-window refused mode=%u logMode=%u valid=%u reserve=%llu partition=%llu imageBlocks=%llu\n",
              storeMode(),logMode(),gStoreGeometry.valid?1:0,reserveBlocks(),partition,(gStoreGeometry.imageBytes+4095)/4096);
        return false;
    }
    uint64_t first=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks;
    IOLockLock(gHost.lock);
    gPicassoRecordWindowOpened=hostLogOpen(first,kLogReserveBlocks+kExportBlocks);
    IOLockUnlock(gHost.lock);
    IOLog("PICASSO_DATA_INIT record-window open=%u first=%llu\n",gPicassoRecordWindowOpened?1:0,first);
    return gPicassoRecordWindowOpened;
}
picasso_data_init::Journal gPicassoDataInit;
uint8_t gPicassoInitScratch[4096],gPicassoInitSaved[4096];
extern "C" void picasso_nvram_periodic_sync(void) __attribute__((weak_import));
extern "C" uint32_t picasso_console_snapshot(void *,uint32_t,uint64_t *,uint64_t *) __attribute__((weak_import));
/* Round 15: one durable console-snapshot writer, usable from any context.
 *
 * The 256 KiB record only ever landed when it was written from the Data-mount
 * path (round 14 probe: raw=1 save=1 generation=1); every save from the log
 * thread has failed since round 12, and the thread's own failure print can only
 * be written into the record that already failed. So keep one Record handle and
 * one pair of buffers here and let the exec path drive it as well: exec is the
 * context the probe proved works, it keeps running long after the USB console
 * dies at ~91 s, and that is what makes the ~210 s stub fallback visible.
 * Returns 1 on success, or a negative stage code. */
static picasso_record::Record gPicassoConsole;
static bool gPicassoConsoleReady;
static uint8_t *gPicassoConsoleData, *gPicassoConsoleScratch;
extern "C" int picasso_console_record_save(const char *tag)
{
    (void)tag;
    if(!&picasso_console_snapshot) return -1;
    if(!gPicassoConsoleData) gPicassoConsoleData=static_cast<uint8_t *>(IOMallocZero(262144));
    if(!gPicassoConsoleScratch) gPicassoConsoleScratch=static_cast<uint8_t *>(IOMallocZero(262144));
    if(!gPicassoConsoleData || !gPicassoConsoleScratch) return -2;
    PicassoRecordOps ops;
    const uint64_t base=gStoreGeometry.last+1-kLogReserveBlocks;
    if(!gPicassoConsoleReady) {
        gPicassoConsole.open(ops,base,2,262144,gPicassoConsoleScratch,gPicassoConsoleData);
        gPicassoConsoleReady=gPicassoConsole.ready;
    }
    if(!gPicassoConsole.ready) return -3;
    uint64_t total=0,dropped=0;
    uint32_t n=picasso_console_snapshot(gPicassoConsoleData,262144,&total,&dropped);
    if(!n) return -4;
    if(!gPicassoConsole.save(ops,gPicassoConsoleData,n,gPicassoConsoleScratch)) {
        /* Round 18: Record::save() latches failed=true on any write error and then
         * refuses every later save. Round 17's boot saved once (the mount probe,
         * generation 54) and then never advanced again for the whole boot, so the
         * channels died on one transient write while the UFS store stayed healthy
         * (the NVRAM record kept its generation). A failed slot write leaves the
         * other slot intact -- Record::save() writes the next slot and bumps the
         * generation only after the header lands -- so clearing the latch and
         * retrying on the next call is safe and keeps the record alive. */
        gPicassoConsole.failed=false;
        static unsigned refusals;
        if(++refusals<=3) IOLog("PICASSO_LOG save retry scheduled refusals=%u\n",refusals);
        return -5;
    }
    return 1;
}
static void picassoLogThread(void *,wait_result_t) {
    /* Round 15: the thread keeps the periodic housekeeping (GPT recheck, NVRAM
     * sync) and calls the shared console-record writer, so there is exactly one
     * Record handle for the console slots instead of two that fight over the
     * same two slots. A refusal is reported for the first two rounds only. */
    unsigned ticks=0;
    while(true) {
        IOSleep(10000);
        if(++ticks%30==0 && native_ufs_store_gpt_check()) {
            IOLockLock(gPicassoRecordLock);gPicassoNvram.failed=true;IOLockUnlock(gPicassoRecordLock);
            IOLog("PICASSO_RECORDS GPT changed; stopped\n");break;
        }
        if(&picasso_nvram_periodic_sync)picasso_nvram_periodic_sync();
        int result=picasso_console_record_save("thread");
        if(result<0 && ticks<=2) IOLog("PICASSO_LOG thread save result=%d\n",result);
    }
}
}
// Exact Picasso layout measured in build164. Root authentication and both GPT
// CRC checks still precede initialization. No writes to the root or GPT.
extern "C" int picasso_data_init_begin(uint64_t pages,const uint8_t seed[32]) {
    uint32_t request=0;PE_parse_boot_argn("picasso-data-replace-20g",&request,sizeof(request));
    IOLog("PICASSO_DATA_INIT enter seed=%u pages=%llu regionBlocks=%llu regionFirst=%llu first=%llu last=%llu imageBytes=%llu reserve=%llu headerCrc=%x entriesCrc=%x replace=%u\n",
          seed?1:0,pages,gStoreRegionBlocks,gStoreRegionFirst,gStoreGeometry.first,gStoreGeometry.last,gStoreGeometry.imageBytes,reserveBlocks(),gStoreGeometry.headerCrc,gStoreGeometry.entriesCrc,request);
    if(!seed || pages!=5242880 || gStoreRegionBlocks!=5243041 || gStoreRegionFirst!=25439066 ||
       gStoreGeometry.first!=2883584 || gStoreGeometry.last!=30944250 ||
       gStoreGeometry.imageBytes!=8380219392ULL || reserveBlocks()!=262144 ||
       gStoreGeometry.headerCrc!=0xfe827362 || gStoreGeometry.entriesCrc!=0xfd3cb4c4 ||
       native_ufs_store_gpt_check() || !picassoRecordWindowOpen())return 0;
    uint8_t identity[picasso_data_init::identityBytes]={};
    picasso_record::put64(identity,gStoreGeometry.first);
    picasso_record::put64(identity+8,gStoreGeometry.last);
    picasso_record::put64(identity+16,gStoreGeometry.imageBytes);
    picasso_record::put64(identity+24,gStoreRegionFirst);
    picasso_record::put64(identity+32,gStoreRegionBlocks);
    picasso_record::put32(identity+40,gStoreGeometry.headerCrc);
    picasso_record::put32(identity+44,gStoreGeometry.entriesCrc);
    picasso_record::put64(identity+48,pages);
    memcpy(identity+56,seed,32);
    PicassoRecordOps ops;
    uint64_t marker=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks+10;
    int phase=gPicassoDataInit.begin(ops,marker,identity,request==1,gPicassoInitScratch,gPicassoInitSaved);
    IOLog("PICASSO_DATA_INIT phase=%d first=%llu blocks=%llu marker=%llu\n",phase,gStoreRegionFirst,gStoreRegionBlocks,marker);
    return phase;
}
extern "C" int picasso_data_init_finish() {
    PicassoRecordOps ops;
    bool ok=!native_ufs_store_gpt_check() && gPicassoDataInit.finish(ops,gPicassoInitScratch);
    IOLog("PICASSO_DATA_INIT complete=%u\n",ok);return ok;
}
extern "C" volatile uint32_t native_data_persist_state;
extern "C" int picasso_persistence_start(void) {
    // A volatile seed fallback does not establish ownership of persistent windows.
    if(native_data_persist_state!=2 && native_data_persist_state!=3) {
        IOLog("PICASSO_RECORDS disabled Data state=%u\n",native_data_persist_state);return 0;
    }
    static bool attempted=false;
    if(attempted)return gPicassoRecordLock && gPicassoNvram.ready;
    attempted=true;
    uint32_t enabled=0;PE_parse_boot_argn("picasso-persist-records",&enabled,sizeof(enabled));
    if(enabled!=1 || storeMode()!=2 || logMode()!=0 || !gStoreGeometry.valid || native_ufs_store_gpt_check())return 0;
    uint64_t partition=gStoreGeometry.last-gStoreGeometry.first+1;
    if(reserveBlocks()<kLogReserveBlocks+kExportBlocks || reserveBlocks()>=partition ||
        partition-reserveBlocks()<(gStoreGeometry.imageBytes+4095)/4096+1024)return 0;
    uint64_t first=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks;
    bool opened=picassoRecordWindowOpen();
    if(!opened || !(gPicassoRecordLock=IOLockAlloc()))return 0;
    PicassoRecordOps ops;
    int state=gPicassoNvram.open(ops,first,1,8192,gPicassoNvramScratch,gPicassoNvramImage);
    gPicassoNvramLoaded=state==1 && gPicassoNvram.length==8192;
    if(state==1 && !gPicassoNvramLoaded)gPicassoNvram.ready=false;
    IOLog("PICASSO_NVRAM open=%d generation=%llu restored=%u\n",state,gPicassoNvram.generation,gPicassoNvramLoaded);
    /* Round 15: one snapshot from the Data-mount path, inside the ~91 s console
     * window, through the shared writer the exec path also uses. Round 14 proved
     * this context can write the full 256 KiB slot (raw=1 save=1 generation=1);
     * printing the verdict here keeps that visible in the USB capture. */
    IOLog("PICASSO_LOG probe result=%d\n", picasso_console_record_save("mount"));
    thread_t thread=THREAD_NULL;
    if(kernel_thread_start(picassoLogThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    return gPicassoNvram.ready;
}
extern "C" int picasso_nvram_load(void *out,uint32_t n) {
    if(!out || n!=8192 || !gPicassoRecordLock)return -1;
    IOLockLock(gPicassoRecordLock);
    int result=gPicassoNvramLoaded?1:gPicassoNvram.ready?0:-1;
    if(result==1)memcpy(out,gPicassoNvramImage,n);
    IOLockUnlock(gPicassoRecordLock);return result;
}
extern "C" int picasso_nvram_save(const void *data,uint32_t n) {
    if(!data || n!=8192 || !gPicassoRecordLock)return 0;
    IOLockLock(gPicassoRecordLock);
    PicassoRecordOps ops;
    bool ok=gPicassoNvram.save(ops,static_cast<const uint8_t *>(data),n,gPicassoNvramScratch);
    if(ok){memcpy(gPicassoNvramImage,data,n);gPicassoNvramLoaded=true;}
    IOLockUnlock(gPicassoRecordLock);
    IOLog("PICASSO_NVRAM save=%u generation=%llu\n",ok,gPicassoNvram.generation);
    return ok;
}

// Ownership marker is outside the auxiliary payload, bound to location and seed.
extern "C" uint8_t native_ram_block_seed_sha256[32];
static bool picassoAuxClaim(uint64_t first,uint64_t blocks) {
    if(!gPicassoRecordLock || blocks!=65536)return false;
    IOLockLock(gPicassoRecordLock);
    if(gAuxRegionBlocks) {
        bool same=gAuxRegionFirst==first && gAuxRegionBlocks==blocks;
        IOLockUnlock(gPicassoRecordLock);return same;
    }
    PicassoRecordOps ops;
    uint8_t *allocation=static_cast<uint8_t *>(IOMallocZero(8192));
    if(!allocation){IOLockUnlock(gPicassoRecordLock);return false;}
    uint8_t *data=allocation,*scratch=allocation+4096;
    picasso_record::Record owner;
    uint64_t marker=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks+6;
    int state=owner.open(ops,marker,3,4096,scratch,data);
    uint8_t expected[48]={};picasso_record::put64(expected,first);picasso_record::put64(expected+8,blocks);
    memcpy(expected+16,native_ram_block_seed_sha256,32);
    bool ok=state==1 && owner.length==48 && !memcmp(data,expected,48);
    if(state==2) {
        uint8_t *scan=static_cast<uint8_t *>(IOMalloc(32*4096));
        ok=scan!=nullptr;
        for(uint64_t b=0;ok && b<blocks;b+=32)ok=ops.read(first+b,32,scan) && picasso_record::zero(scan,32*4096);
        if(scan)IOFree(scan,32*4096);
        if(ok)ok=!native_ufs_store_gpt_check() && owner.save(ops,expected,48,scratch);
    }
    if(ok) {IOLockLock(gHost.lock);ok=hostAuxOpen(first,blocks);IOLockUnlock(gHost.lock);}
    if(ok) {gAuxRegionFirst=first;__atomic_store_n(&gAuxRegionBlocks,blocks,__ATOMIC_RELEASE);}
    IOFree(allocation,8192);IOLockUnlock(gPicassoRecordLock);return ok;
}
