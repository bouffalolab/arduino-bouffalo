#include "tcp_rx_credit_pool.h"
#include <assert.h>
#include <stdint.h>
#include <initializer_list>
using bridge::TcpRxCreditPool;
typedef TcpRxCreditPool Pool;
static Pool::Grant open(Pool &pool, uintptr_t pcb) {
    auto grant = pool.open(pcb);
    assert(grant.token && grant.bytes == 256 && pool.commit(grant));
    return grant;
}
static void check(const Pool &pool) {
    const auto s = pool.snapshot();
    assert(s.sequence_used <= 12288 && s.minimum_reserved <= 12288 - s.sequence_used);
    assert(s.future_reserved <= s.minimum_reserved && s.physical_used <= 24576);
    assert(s.live_flows + s.retired_flows <= Pool::FlowCount && s.blocks <= Pool::BlockCount);
    assert(pool.healthy());
}
int main() {
    {
        Pool pool(12288,24576,256,2048,256);
        auto first = open(pool,1);
        assert(pool.request(first.token,2048));
        for (;;) {
            auto grant = pool.next_grant(); if (!grant.token) break;
            assert(pool.commit(grant));
        }
        // Early flows cannot consume the minimum of the 31 future slots.
        assert(pool.snapshot().sequence_used==2048);
        assert(pool.request(first.token,0) && !pool.next_grant().token);
        assert(pool.received(first.token,256) && pool.recved(first.token,256));
        assert(!pool.next_grant().token); // suspension leaves old promises valid
        for(unsigned i=2;i<=32;++i) open(pool,i);
        check(pool);
    }
    {
        Pool pool(12288,24576,256,2048,256);
        Pool::Token flows[32];
        for (unsigned i=0;i<32;++i) flows[i]=open(pool,100+i).token;
        assert(!pool.open(500).token);
        assert(pool.snapshot().sequence_used==8192);
        // All 32 contenders receive one equal quantum before any second grant.
        for(auto token:flows) assert(pool.request(token,2048));
        for(unsigned i=0;i<16;++i) {
            auto grant=pool.next_grant(); assert(grant.token==flows[i] && grant.bytes==256);
            assert(pool.commit(grant));
        }
        assert(!pool.next_grant().token && pool.snapshot().sequence_used==12288);
        // Return sequence credit while physical allocation is still retained.
        assert(pool.retain(flows[0],10000,1052));
        assert(pool.received(flows[0],512) && pool.recved(flows[0],511));
        assert(pool.snapshot().physical_used==1052);
        auto grant=pool.next_grant(); assert(grant.token==flows[16] && grant.bytes==256);
        assert(pool.commit(grant));
        // This flow's own minimum remains reserved; it can reopen even when
        // discretionary bytes have all been promised to other flows.
        auto own=pool.next_grant(); assert(own.token==flows[0] && own.bytes==255);
        assert(pool.commit(own)); check(pool);
        assert(pool.retire(flows[0])); check(pool);
        assert(pool.snapshot().retired_flows==1 && pool.snapshot().physical_used==1052);
        // PCB address reuse has a fresh token. Old final-free affects old owner.
        assert(!pool.open(100).token); // all metadata occupied by live/retired owners
        assert(pool.retire(flows[31]));
        auto replacement=open(pool,100); assert(replacement.token!=flows[0]);
        assert(pool.resize(10000,100) && pool.snapshot().physical_used==100);
        assert(pool.resize(10000,0) && pool.snapshot().retired_flows==0);
        check(pool);
    }
    {
        Pool pool(12288,24576,256,2048,256);
        auto a=open(pool,1), b=open(pool,2);
        assert(pool.request(a.token,2048) && pool.request(b.token,2048));
        auto ga=pool.next_grant(); assert(ga.token==a.token && pool.cancel(ga));
        auto gb=pool.next_grant(); assert(gb.token==b.token && pool.commit(gb));
        auto newer=pool.next_grant(); assert(newer.token==a.token && newer.ticket!=ga.ticket);
        assert(!pool.commit(ga)); // stale transaction cannot commit a new grant
        assert(!pool.next_grant().token); // fail closed, retain cleanup capability
        assert(pool.cancel(newer) && pool.retire(a.token) && pool.retire(b.token));
        assert(pool.snapshot().sequence_used==0);
    }
    {
        Pool pool(12288,24576,256,2048,256);
        auto a=open(pool,1);
        for(unsigned i=0;i<128;++i) assert(pool.retain(a.token,1000+i,16));
        assert(!pool.retain(a.token,2000,16)); // metadata exhausted, no untracked credit
        assert(pool.snapshot().blocks==128 && pool.snapshot().physical_used==2048);
        assert(pool.retire(a.token));
        for(unsigned i=0;i<128;++i) assert(pool.resize(1000+i,0));
        assert(pool.snapshot().retired_flows==0 && pool.snapshot().physical_used==0);
        check(pool);
    }
    {
        Pool pool(12288,24576,256,2048,256);
        auto a=open(pool,1);
        assert(pool.retain(a.token,1000,24576));
        assert(!pool.retain(a.token,1001,1));
        assert(pool.resize(1000,24575) && pool.retain(a.token,1001,1));
        assert(pool.retire(a.token) && pool.snapshot().physical_used==24576);
        assert(pool.resize(1000,0) && pool.resize(1001,0)); check(pool);
    }
    {
        // Repeated complete receive/read/retire with another flow keeping the
        // discretionary pool full must preserve the next connection's minimum.
        Pool pool(12288,24576,256,2048,256);
        Pool::Token flows[32];
        for(unsigned i=0;i<32;++i) flows[i]=open(pool,1+i).token;
        for(auto token:flows) assert(pool.request(token,2048));
        for(unsigned round=0;round<1000;++round) {
            for(;;) { auto g=pool.next_grant(); if(!g.token) break; assert(pool.commit(g)); }
            const unsigned i=round%32;
            // Track the full grant by retiring/reopening after a minimum read.
            assert(pool.received(flows[i],256) && pool.recved(flows[i],256));
            assert(pool.retire(flows[i]));
            flows[i]=open(pool,1+i).token;
            assert(pool.request(flows[i],2048)); check(pool);
        }
    }
    {
        Pool pool(12288,24576,256,2048,256);
        auto a=open(pool,1);
        assert(pool.lifecycle(2000,0,100)); // unbound allocation stays uncharged
        assert(pool.retain(a.token,2000,100) && pool.allocation_charge(2000)==100);
        assert(pool.lifecycle(2000,100,60) && pool.snapshot().physical_used==60);
        assert(pool.retire(a.token));
        // A bad observer transition poisons grants, but real final free must
        // still clear the old owner's block and retired generation.
        assert(!pool.lifecycle(2000,61,0));
        assert(!pool.healthy() && !pool.snapshot().physical_used && !pool.snapshot().retired_flows);
        assert(pool.lifecycle(2001,100,0)); // unrelated free cannot credit a flow
    }
    {
        // A non-consuming tiny-segment sender cannot exhaust bytes or block
        // records reserved for all existing and future owner slots.
        for (unsigned base=256;base<=512;base+=256) {
        Pool pool(12288,20480,256,768,256,base,4096,16);
        auto attacker=open(pool,1);
        for (unsigned n=0;n<16;++n) assert(pool.retain(attacker.token,1000+n,64));
        assert(!pool.retain(attacker.token,2000,64));
        assert(pool.physical_reserved()+pool.snapshot().physical_used<=20480);
        Pool::Token peers[31];
        for(unsigned n=0;n<31;++n) {
            peers[n]=open(pool,2+n).token;
            assert(pool.retain(peers[n],3000+n,512));
        }
        // Application reads return sequence only, so the attacker remains capped.
        assert(pool.received(attacker.token,1) && pool.recved(attacker.token,1));
        assert(!pool.retain(attacker.token,2000,64));
        for(unsigned round=0;round<1000;++round) {
            const unsigned n=round%31;
            assert(pool.lifecycle(3000+n,512,0));
            assert(pool.retain(peers[n],3000+n,512));
            assert(pool.physical_reserved()+pool.snapshot().physical_used<=20480);
            assert(pool.blocks_reserved()+pool.snapshot().blocks<=128);
        }
        assert(pool.retire(attacker.token));
        assert(pool.physical_reserved()+pool.snapshot().physical_used<=20480);
        assert(pool.snapshot().retired_flows==1);
        for(unsigned n=0;n<16;++n) assert(pool.lifecycle(1000+n,64,0));
        assert(pool.snapshot().retired_flows==0);
        auto next=open(pool,1);assert(next.token!=attacker.token);
        assert(pool.retain(next.token,9999,4096));
        assert(!pool.retain(next.token,9998,1));
        assert(pool.lifecycle(9999,4096,2048));
        assert(pool.retain(next.token,9998,512));
        check(pool);
        }
    }
    {
        // Observed native charges: a plain 64 B reply costs 144 B and a
        // full 768 B window costs 848 B. A partially consumed native buffer
        // can remain referenced after recved, while its next window arrives.
        // Reserve policy must accommodate this ordinary 24-owner workload,
        // without borrowing the remaining eight owners' protected bases.
        for (unsigned base=256;base<=512;base+=256) {
            Pool pool(12288,20480,256,768,256,base,4096,16);
            Pool::Token tls[8];
            for (unsigned n=0;n<16;++n) {
                auto plain=open(pool,1+n);
                assert(pool.retain(plain.token,1000+n,144));
            }
            for (unsigned n=0;n<8;++n) {
                tls[n]=open(pool,17+n).token;
                assert(pool.retain(tls[n],2000+n,848));
            }
            unsigned admitted=0;
            for (unsigned n=0;n<8;++n)
                admitted+=pool.retain(tls[n],3000+n,848);
            if (base==256) {
                assert(admitted==8 && pool.snapshot().physical_used==15872);
                assert(pool.physical_reserved()==3840);
            } else {
                assert(admitted<8); // Reproduces premature 512 B backpressure.
            }
            check(pool);
            assert(pool.physical_reserved()+pool.snapshot().physical_used<=20480);
        }
    }
    {
        // Global reserve remains effective with several large non-consumers;
        // admission denial is normal backpressure, not an accounting fault.
        Pool pool(12288,20480,256,768,256,512,4096,16);
        auto a=open(pool,1),b=open(pool,2),c=open(pool,3);
        assert(pool.retain(a.token,1000,4096));
        assert(!pool.retain(b.token,1001,4096));
        assert(pool.retain(b.token,1001,1024));
        assert(!pool.retain(c.token,1002,513));
        assert(pool.retain(c.token,1002,512));
        assert(pool.retire(a.token));
        assert(pool.physical_reserved()+pool.snapshot().physical_used==20480);
        assert(pool.lifecycle(1000,4096,0));
        assert(pool.retain(c.token,1003,1024));
        assert(pool.healthy());
    }
    {
        // Multiple tiny non-consumers: seven owners cannot each retain 16
        // blocks because 25 future/current empty owners still need one each.
        // Prove the actual admission boundary, then keep a healthy flow moving
        // without reclaiming unread attacker allocations.
        for (unsigned bad_count : {2U, 4U, 7U}) {
            Pool pool(12288,20480,256,768,256,256,4096,16);
            Pool::Token bad[7];
            unsigned retained[7] = {};
            for (unsigned i=0;i<bad_count;++i) {
                bad[i]=open(pool,100+i).token;
                for (unsigned n=0;n<16;++n) {
                    if (!pool.retain(bad[i],1000+i*32+n,80)) break;
                    ++retained[i];
                    assert(pool.received(bad[i],1));
                }
                assert(!pool.retain(bad[i],1000+i*32+16,80));
            }
            const unsigned expected=bad_count==7 ? 103 : bad_count*16;
            assert(pool.snapshot().blocks==expected);
            auto good=open(pool,999);
            for (unsigned round=0;round<1000;++round) {
                assert(pool.retain(good.token,9999,144));
                assert(pool.received(good.token,64) && pool.recved(good.token,64));
                assert(pool.lifecycle(9999,144,0));
                while (true) {
                    auto grant=pool.next_grant();
                    if (!grant.token) break;
                    assert(grant.token==good.token && pool.commit(grant));
                }
                assert(pool.snapshot().blocks==expected);
                assert(pool.physical_reserved()+pool.snapshot().physical_used<=20480);
                assert(pool.blocks_reserved()+pool.snapshot().blocks<=128);
                check(pool);
            }
            // Retiring all attackers preserves all real physical charges.
            for (unsigned i=0;i<bad_count;++i) assert(pool.retire(bad[i]));
            assert(pool.snapshot().retired_flows==bad_count);
            assert(pool.snapshot().blocks==expected);
            assert(pool.retire(good.token));
            assert(!pool.snapshot().sequence_used);
            for (unsigned i=0;i<bad_count;++i)
                for (unsigned n=0;n<retained[i];++n)
                    assert(pool.lifecycle(1000+i*32+n,80,0));
            auto final=pool.snapshot();
            assert(!final.physical_used && !final.blocks && !final.live_flows && !final.retired_flows);
            assert(pool.physical_reserved()==8192 && pool.blocks_reserved()==32);
            // Same PCB addresses acquire fresh generations and full quotas.
            for (unsigned i=0;i<bad_count;++i) {
                auto reopened=open(pool,100+i);
                assert(reopened.token!=bad[i]);
                assert(pool.retain(reopened.token,1000+i,80));
                assert(pool.lifecycle(1000+i,80,0) && pool.retire(reopened.token));
            }
            check(pool);
        }
    }
    {
        Pool invalid(8191,24576,256,2048,256);
        assert(!invalid.healthy() && !invalid.open(1).token);
        Pool pool(12288,24576,256,2048,256);
        auto a=open(pool,1);
        assert(!pool.received(a.token,257));
        assert(!pool.next_grant().token && pool.retire(a.token));
    }
}
