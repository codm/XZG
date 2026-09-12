#pragma once
#include <stdint.h>
#include <string.h>

namespace czc {
// The radio applies pending bootstrap profiles (1) at BDB startup. Committed
// profiles (2) identify the managing Master; migration records (3) persist the
// intent to clear the Satellite network and request that Master's current profile.
class SatelliteMigration {
public:
    enum : uint16_t { Boot=0x03f0, Nib=0x0021, Startup=3, OnNetwork=0x55, RecordSize=88 };
    enum Result { Unmanaged, StorageError, Prepared, ResetRequired, Cleared };
    struct Nv {
        virtual bool length(uint16_t id,uint16_t &size)=0;
        virtual bool read(uint16_t id,uint8_t *data,unsigned size)=0;
        virtual bool write(uint16_t id,const uint8_t *data,unsigned size)=0;
        virtual bool remove(uint16_t id,uint16_t size)=0;
        virtual ~Nv() {}
    };
    static bool networkMatches(const uint8_t *a,const uint8_t *b) {
        return !memcmp(a+8,b+8,3) && !memcmp(a+20,b+20,8);
    }
    static Result prepare(Nv &nv,const uint8_t local[32],const uint8_t master[32]) {
        // Never adopt an arbitrary running coordinator/router just because it
        // knows the group PSK. It must have a committed profile from this IEEE.
        if(local[11]!=7 || master[11]!=9 || master[6] || master[7] ||
           master[10]<11 || master[10]>26 || networkMatches(local,master)) return Unmanaged;
        uint8_t record[RecordSize]{}; uint16_t size=0;
        if(!nv.length(Boot,size)) return StorageError;
        if(size!=RecordSize) return Unmanaged;
        if(!nv.read(Boot,record,sizeof(record))) return StorageError;
        const uint8_t *p=record+5;
        bool managed=valid(record) && record[4]==2 && p[0]==1 &&
            !memcmp(p+8,master+12,8) && !memcmp(p+16,local+12,8) &&
            p[1]==local[10] && !memcmp(p+2,local+8,2) && !memcmp(p+24,local+20,8);
        if(!managed) { wipe(record,sizeof(record)); return Unmanaged; }
        // Committed profiles have already had their keys wiped by the radio.
        // Persist the intent BEFORE setting startup flags or removing any NV.
        record[4]=3;
        bool ok=put(nv,Boot,record,sizeof(record)); wipe(record,sizeof(record));
        return ok ? Prepared : StorageError;
    }
    static Result advance(Nv &nv,const uint8_t local[32]) {
        uint8_t record[RecordSize]{}; uint16_t size=0;
        if(!nv.length(Boot,size)) return StorageError;
        if(size!=RecordSize) return Unmanaged;
        if(!nv.read(Boot,record,sizeof(record))) return StorageError;
        bool pending=valid(record) && record[4]==3 && record[5]==1 &&
            !memcmp(record+21,local+12,8);
        wipe(record,sizeof(record));
        if(!pending || local[11]==9) return Unmanaged;
        uint8_t on=0,options=0;
        if(!nv.read(OnNetwork,&on,1) || !nv.read(Startup,&options,1)) return StorageError;
        if(on || local[11]==7 || (options&1)) {
            // TI zgInit clears configuration, BDB membership and old link keys.
            // Deliberately exclude 0x80: retain NWK frame-counter high-water marks.
            uint8_t reset=3;
            return put(nv,Startup,&reset,1) ? ResetRequired : StorageError;
        }
        // Only after the reset: a live network could otherwise save its NIB
        // again between delete and reset. BDB initialization is held by caller.
        if(!eraseItem(nv,Nib) || !eraseItem(nv,Boot)) return StorageError;
        return Cleared;
    }
private:
    static bool valid(const uint8_t *r) { return r[0]==0x32 && r[1]==0x43 && r[2]==0x48 && r[3]==0x42; }
    static void wipe(void *data,unsigned n) { volatile uint8_t *p=static_cast<volatile uint8_t *>(data); while(n--) *p++=0; }
    static bool put(Nv &nv,uint16_t id,const uint8_t *data,unsigned size) {
        uint8_t verify[RecordSize]{};
        bool ok=nv.write(id,data,size) && nv.read(id,verify,size) && !memcmp(data,verify,size);
        wipe(verify,sizeof(verify)); return ok;
    }
    static bool eraseItem(Nv &nv,uint16_t id) {
        uint16_t size=0,after=0;
        return nv.length(id,size) && (!size || (nv.remove(id,size) && nv.length(id,after) && !after));
    }
};
}
