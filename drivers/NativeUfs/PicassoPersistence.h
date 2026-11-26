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
static void picassoLogThread(void *,wait_result_t) {
    uint8_t *scratch=static_cast<uint8_t *>(IOMallocZero(262144));
    uint8_t *data=static_cast<uint8_t *>(IOMallocZero(262144));
    if(!scratch || !data) {if(scratch)IOFree(scratch,262144);if(data)IOFree(data,262144);thread_terminate(current_thread());return;}
    PicassoRecordOps ops;
    uint64_t base=gStoreGeometry.last+1-kLogReserveBlocks;
    int opened=gPicassoLog.open(ops,base,2,262144,scratch,data);
    // Round 12: a window holding neither a valid record nor zeros is not a
    // journal, but Record::open() deliberately leaves it usable (ready=true,
    // returns 0) so an authorized caller can overwrite it -- the same rule the
    // Data journal needed in round 5. Gating the whole log on opened!=0 threw
    // that away: this boot printed PICASSO_LOG open=0 and then never recorded
    // a byte, although the console ring and the durable window were both
    // healthy. Keep skipping only when the window is unusable (I/O failure,
    // ready=false), and say which case it was.
    IOLog("PICASSO_LOG open=%d generation=%llu previous_bytes=%u ready=%u\n",
          opened,gPicassoLog.generation,gPicassoLog.length,gPicassoLog.ready?1:0);
    if(!opened && gPicassoLog.ready)
        IOLog("PICASSO_LOG adopting unformatted window first=%llu\n",base);
    uint64_t previous=0;unsigned ticks=0;
    while(true) {
        IOSleep(10000);
        // Same periodic GPT recheck rule as Sheng BootLog: stop records if
        // the allocation changed underneath us. No transport reset/retry.
        if(++ticks%30==0 && native_ufs_store_gpt_check()) {
            IOLockLock(gPicassoRecordLock);gPicassoNvram.failed=true;IOLockUnlock(gPicassoRecordLock);
            IOLog("PICASSO_RECORDS GPT changed; stopped\n");break;
        }
        if(&picasso_nvram_periodic_sync)picasso_nvram_periodic_sync();
        if((!opened && !gPicassoLog.ready) || !&picasso_console_snapshot)continue;
        uint64_t total=0,dropped=0;
        uint32_t n=picasso_console_snapshot(data,262144,&total,&dropped);
        if(!n || total==previous)continue;
        if(!gPicassoLog.save(ops,data,n,scratch)) {IOLog("PICASSO_LOG save failed; stopped\n");opened=0;gPicassoLog.ready=false;continue;}
        previous=total;
        // No per-save console logging: that would create its own new payload.
    }
    IOFree(scratch,262144);IOFree(data,262144);
    thread_terminate(current_thread());
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
    /* Round 13/14: prove the console record while the USB console is still alive.
     * Round 12 adopted the window and then produced no record at all, and the only
     * diagnostic the thread has ("save failed") can only be written into the record
     * that already failed. This probe runs in the Data-mount path, well inside the
     * ~91 s console window. Round 13's probe called save() before the record had
     * ever been opened (capacity 0, so save refused by construction) -- it still
     * proved the console ring is being filled (n=262144, total=572859). Round 14
     * opens the record first and separates the three failure modes: a bare raw
     * block write (window/gating), a full 256 KiB slot save, and a 8 KiB save
     * (size-dependent refusal). */
    {
        uint8_t *probe=static_cast<uint8_t *>(IOMallocZero(262144));
        uint8_t *probeScratch=static_cast<uint8_t *>(IOMallocZero(262144));
        if(probe && probeScratch) {
            const uint64_t logBase=gStoreGeometry.last+1-kLogReserveBlocks;
            int probeOpen=gPicassoLog.open(ops,logBase,2,262144,probeScratch,probe);
            memset(probe,0,4096);
            bool raw=ops.write(logBase+2,1,probe);
            uint64_t total=0,dropped=0;
            uint32_t n=&picasso_console_snapshot?picasso_console_snapshot(probe,262144,&total,&dropped):0;
            bool saved=n!=0 && gPicassoLog.ready && gPicassoLog.save(ops,probe,n,probeScratch);
            bool savedSmall=false;
            if(!saved && n) {
                gPicassoLog.failed=false;
                savedSmall=gPicassoLog.ready && gPicassoLog.save(ops,probe,8192,probeScratch);
            }
            IOLog("PICASSO_LOG probe opened=%d ready=%u n=%u total=%llu dropped=%llu raw=%u save=%u save8k=%u generation=%llu failed=%u\n",
                  probeOpen,gPicassoLog.ready?1U:0U,n,(unsigned long long)total,(unsigned long long)dropped,
                  raw?1U:0U,saved?1U:0U,savedSmall?1U:0U,(unsigned long long)gPicassoLog.generation,
                  gPicassoLog.failed?1U:0U);
        } else {
            IOLog("PICASSO_LOG probe unavailable probe=%u scratch=%u interface=%u\n",
                  probe?1U:0U,probeScratch?1U:0U,&picasso_console_snapshot?1U:0U);
        }
        if(probe)IOFree(probe,262144);
        if(probeScratch)IOFree(probeScratch,262144);
    }
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
