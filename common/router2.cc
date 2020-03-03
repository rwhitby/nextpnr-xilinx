/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  Core routing algorithm based on CRoute:
 *
 *     CRoute: A Fast High-quality Timing-driven Connection-based FPGA Router
 *     Dries Vercruyce, Elias Vansteenkiste and Dirk Stroobandt
 *     DOI 10.1109/FCCM.2019.00017 [PDF on SciHub]
 *
 *  Modified for the nextpnr Arch API and data structures; optimised for
 *  real-world FPGA architectures in particular ECP5 and Xilinx UltraScale+
 *
 */

#include "router2.h"
#include "router2_int.h"

#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <queue>
#include <thread>
#include "log.h"
#include "nextpnr.h"
#include "router1.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Router2 {

ArcRouteResult Router2ArchFunctions::route_segment(Router2Thread *th, const NetInfo *net, size_t seg_idx, bool is_mt,
                                                   bool no_bb)
{
    return ARC_USE_DEFAULT;
}

std::vector<NetSegment> Router2ArchFunctions::segment_net(NetInfo *net)
{
    std::vector<NetSegment> segments;
    for (size_t i = 0; i < net->users.size(); i++) {
        segments.emplace_back(r->ctx, net, i);
    }
    return segments;
}

float Router2State::present_wire_cost(const Router2State::PerWireData &w, int net_uid)
{
    int other_sources = int(w.bound_nets.size());
    if (w.bound_nets.count(net_uid))
        other_sources -= 1;
    if (other_sources == 0)
        return 1.0f;
    else
        return 1 + other_sources * curr_cong_weight;
}
void Router2State::setup_nets()
{
    // Populate per-net and per-arc structures at start of routing
    nets.resize(ctx->nets.size());
    nets_by_udata.resize(ctx->nets.size());
    size_t i = 0;
    for (auto net : sorted(ctx->nets)) {
        NetInfo *ni = net.second;
        ni->udata = i;
        nets_by_udata.at(i) = ni;
        auto segments = f->segment_net(ni);
        nets.at(i).segments.resize(segments.size());

        // Start net bounding box at overall min/max
        nets.at(i).bb.x0 = std::numeric_limits<int>::max();
        nets.at(i).bb.x1 = std::numeric_limits<int>::min();
        nets.at(i).bb.y0 = std::numeric_limits<int>::max();
        nets.at(i).bb.y1 = std::numeric_limits<int>::min();
        nets.at(i).cx = 0;
        nets.at(i).cy = 0;

        if (ni->driver.cell != nullptr) {
            Loc drv_loc = ctx->getBelLocation(ni->driver.cell->bel);
            nets.at(i).cx += drv_loc.x;
            nets.at(i).cy += drv_loc.y;
        }

        for (size_t j = 0; j < segments.size(); j++) {
            auto &seg = segments.at(j);
            if (ni->driver.cell == nullptr)
                seg.src_wire = seg.dst_wire;
            if (ni->driver.cell == nullptr && seg.dst_wire == WireId())
                continue;
            nets.at(i).segments.at(j).s = seg;
            // Set bounding box for this arc
            nets.at(i).segments.at(j).bb = ctx->getRouteBoundingBox(seg.src_wire, seg.dst_wire);
            // Expand net bounding box to include this arc
            nets.at(i).bb.x0 = std::min(nets.at(i).bb.x0, nets.at(i).segments.at(j).bb.x0);
            nets.at(i).bb.x1 = std::max(nets.at(i).bb.x1, nets.at(i).segments.at(j).bb.x1);
            nets.at(i).bb.y0 = std::min(nets.at(i).bb.y0, nets.at(i).segments.at(j).bb.y0);
            nets.at(i).bb.y1 = std::max(nets.at(i).bb.y1, nets.at(i).segments.at(j).bb.y1);
        }
        for (auto &usr : ni->users) {
            // Add location to centroid sum
            Loc usr_loc = ctx->getBelLocation(usr.cell->bel);
            nets.at(i).cx += usr_loc.x;
            nets.at(i).cy += usr_loc.y;
        }
        nets.at(i).hpwl = std::max(
                std::abs(nets.at(i).bb.y1 - nets.at(i).bb.y0) + std::abs(nets.at(i).bb.x1 - nets.at(i).bb.x0), 1);
        nets.at(i).cx /= int(ni->users.size() + 1);
        nets.at(i).cy /= int(ni->users.size() + 1);
        if (ctx->debug)
            log_info("%s: bb=(%d, %d)->(%d, %d) c=(%d, %d) hpwl=%d\n", ctx->nameOf(ni), nets.at(i).bb.x0,
                     nets.at(i).bb.y0, nets.at(i).bb.x1, nets.at(i).bb.y1, nets.at(i).cx, nets.at(i).cy,
                     nets.at(i).hpwl);
        i++;
    }
}

void Router2State::setup_wires()
{
    // Set up per-wire structures, so that MT parts don't have to do any memory allocation
    // This is possibly quite wasteful and not cache-optimal; further consideration necessary
    for (auto wire : ctx->getWires()) {
        PerWireData pwd;
        pwd.w = wire;
        NetInfo *bound = ctx->getBoundWireNet(wire);
        if (bound != nullptr) {
            pwd.bound_nets[bound->udata] = std::make_pair(1, bound->wires.at(wire).pip);
            if (bound->wires.at(wire).strength > STRENGTH_STRONG)
                pwd.unavailable = true;
        }
        wire_to_idx[wire] = int(flat_wires.size());
        flat_wires.push_back(pwd);
    }
}

bool Router2State::hit_test_pip(ArcBounds &bb, Loc l)
{
    return l.x >= (bb.x0 - cfg.bb_margin_x) && l.x <= (bb.x1 + cfg.bb_margin_x) && l.y >= (bb.y0 - cfg.bb_margin_y) &&
           l.y <= (bb.y1 + cfg.bb_margin_y);
}

// Define to make sure we don't print in a multithreaded context
#define ARC_LOG_ERR(...)                                                                                               \
    do {                                                                                                               \
        if (is_mt)                                                                                                     \
            return ARC_FATAL;                                                                                          \
        else                                                                                                           \
            log_error(__VA_ARGS__);                                                                                    \
    } while (0)
#define ROUTE_LOG_DBG(...)                                                                                             \
    do {                                                                                                               \
        if (!is_mt && ctx->debug)                                                                                      \
            log(__VA_ARGS__);                                                                                          \
    } while (0)

void Router2State::bind_pip_internal(NetInfo *net, int wire, PipId pip)
{
    auto &b = flat_wires.at(wire).bound_nets[net->udata];
    ++b.first;
    if (b.first == 1) {
        b.second = pip;
    } else {
        NPNR_ASSERT(b.second == pip);
    }
}

void Router2State::unbind_pip_internal(NetInfo *net, WireId wire)
{
    auto &b = wire_data(wire).bound_nets.at(net->udata);
    --b.first;
    if (b.first == 0) {
        wire_data(wire).bound_nets.erase(net->udata);
    }
}

void Router2State::ripup_seg(NetInfo *net, size_t s)
{
    auto &sd = nets.at(net->udata).segments.at(s);
    if (!sd.routed)
        return;
    WireId src = sd.s.src_wire;
    WireId cursor = sd.s.dst_wire;
    while (cursor != src) {
        auto &wd = wire_data(cursor);
        PipId pip = wd.bound_nets.at(net->udata).second;
        unbind_pip_internal(net, cursor);
        cursor = ctx->getPipSrcWire(pip);
    }
    sd.routed = false;
}

float Router2State::score_wire_for_net(NetInfo *net, WireId wire, PipId pip)
{
    auto &wd = wire_data(wire);
    auto &nd = nets.at(net->udata);
    float base_cost = ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                                      ctx->getDelayEpsilon());
    float present_cost = present_wire_cost(wd, net->udata);
    float hist_cost = wd.hist_cong_cost;
    float bias_cost = 0;
    int source_uses = 0;
    if (wd.bound_nets.count(net->udata))
        source_uses = wd.bound_nets.at(net->udata).first;
    if (pip != PipId()) {
        Loc pl = ctx->getPipLocation(pip);
        bias_cost = cfg.bias_cost_factor * (base_cost / int(net->users.size())) *
                    ((std::abs(pl.x - nd.cx) + std::abs(pl.y - nd.cy)) / float(nd.hpwl));
    }
    return base_cost * hist_cost * present_cost / (1 + source_uses) + bias_cost;
}

float Router2State::get_togo_cost(NetInfo *net, int wire, WireId sink)
{
    auto &wd = flat_wires[wire];
    int source_uses = 0;
    if (wd.bound_nets.count(net->udata))
        source_uses = wd.bound_nets.at(net->udata).first;
    // FIXME: timing/wirelength balance?
    return (ctx->getDelayNS(ctx->estimateDelay(wd.w, sink)) / (1 + source_uses)) + cfg.ipin_cost_adder;
}

bool Router2State::check_seg_routing(NetInfo *net, size_t s)
{
    auto &sd = nets.at(net->udata).segments.at(s);
    WireId src_wire = sd.s.src_wire;
    WireId cursor = sd.s.dst_wire;
    while (wire_data(cursor).bound_nets.count(net->udata)) {
        auto &wd = wire_data(cursor);
        if (wd.bound_nets.size() != 1)
            return false;
        auto &uh = wd.bound_nets.at(net->udata).second;
        if (uh == PipId())
            break;
        cursor = ctx->getPipSrcWire(uh);
    }
    return (cursor == src_wire);
}

// Returns true if a wire contains no source ports or driving pips
bool Router2State::is_wire_undriveable(WireId wire)
{
    for (auto bp : ctx->getWireBelPins(wire))
        if (ctx->getBelPinType(bp.bel, bp.pin) != PORT_IN)
            return false;
    for (auto p : ctx->getPipsUphill(wire))
        return false;
    return true;
}

// Find all the wires that must be used to route a given arc
void Router2State::reserve_wires_for_arc(NetInfo *net, size_t i)
{
    WireId src = ctx->getNetinfoSourceWire(net);
    WireId sink = ctx->getNetinfoSinkWire(net, net->users.at(i));
    if (sink == WireId())
        return;
    std::unordered_set<WireId> rsv;
    WireId cursor = sink;
    bool done = false;
    if (ctx->debug)
        log("resevering wires for arc %d of net %s\n", int(i), ctx->nameOf(net));
    while (!done) {
        auto &wd = wire_data(cursor);
        if (ctx->debug)
            log("      %s\n", ctx->nameOfWire(cursor));
        wd.reserved_net = net->udata;
        if (cursor == src)
            break;
        WireId next_cursor;
        for (auto uh : ctx->getPipsUphill(cursor)) {
            WireId w = ctx->getPipSrcWire(uh);
            if (is_wire_undriveable(w))
                continue;
            if (next_cursor != WireId()) {
                done = true;
                break;
            }
            next_cursor = w;
        }
        if (next_cursor == WireId())
            break;
        cursor = next_cursor;
    }
}

void Router2State::find_all_reserved_wires()
{
    for (auto net : nets_by_udata) {
        WireId src = ctx->getNetinfoSourceWire(net);
        if (src == WireId())
            continue;
        for (size_t i = 0; i < net->users.size(); i++)
            reserve_wires_for_arc(net, i);
    }
}

void Router2State::reset_wires(Router2Thread &t)
{
    for (auto w : t.dirty_wires) {
        flat_wires[w].visit.visited = false;
        flat_wires[w].visit.dirty = false;
        flat_wires[w].visit.pip = PipId();
        flat_wires[w].visit.score = WireScore();
    }
    t.dirty_wires.clear();
}

void Router2State::set_visited(Router2Thread &t, int wire, PipId pip, WireScore score)
{
    auto &v = flat_wires.at(wire).visit;
    if (!v.dirty)
        t.dirty_wires.push_back(wire);
    v.dirty = true;
    v.visited = true;
    v.pip = pip;
    v.score = score;
}

#if 0
    // Special-case constant ground/vcc routing for Xilinx devices
    void route_xilinx_const(Router2Thread &t, NetInfo *net, size_t i, int src_wire_idx, WireId dst_wire, bool is_mt,
                            bool is_bb = true)
    {
        auto &nd = nets[net->udata];
        auto &ad = nd.arcs[i];

        int backwards_iter = 0;
        int backwards_limit = 5000000;

        bool const_val = false;
        if (net->name == ctx->id("$PACKER_VCC_NET"))
            const_val = true;
        else
            NPNR_ASSERT(net->name == ctx->id("$PACKER_GND_NET"));

        for (int allowed_cong = 0; allowed_cong < 10; allowed_cong++) {
            backwards_iter = 0;
            if (!t.backwards_queue.empty()) {
                std::queue<int> new_queue;
                t.backwards_queue.swap(new_queue);
            }
            t.backwards_queue.push(wire_to_idx.at(dst_wire));
            reset_wires(t);
            while (!t.backwards_queue.empty() && backwards_iter < backwards_limit) {
                int cursor = t.backwards_queue.front();
                t.backwards_queue.pop();
                auto &cwd = flat_wires[cursor];
                PipId cpip;
                if (cwd.bound_nets.count(net->udata)) {
                    // If we can tack onto existing routing; try that
                    // Only do this if the existing routing is uncontented; however
                    int cursor2 = cursor;
                    bool bwd_merge_fail = false;
                    while (flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                        if (int(flat_wires.at(cursor2).bound_nets.size()) > (allowed_cong + 1)) {
                            bwd_merge_fail = true;
                            break;
                        }
                        PipId p = flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                        if (p == PipId())
                            break;
                        cursor2 = wire_to_idx.at(ctx->getPipSrcWire(p));
                    }
                    if (!bwd_merge_fail && cursor2 == src_wire_idx) {
                        // Found a path to merge to existing routing; backwards
                        cursor2 = cursor;
                        while (flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                            PipId p = flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                            if (p == PipId())
                                break;
                            cursor2 = wire_to_idx.at(ctx->getPipSrcWire(p));
                            set_visited(t, cursor2, p, WireScore());
                        }
                        break;
                    }
                    cpip = cwd.bound_nets.at(net->udata).second;
                }
#if 0
                log("   explore %s\n", ctx->nameOfWire(cwd.w));
#endif
                if (ctx->wireIntent(cwd.w) == (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND)) {
#if 0
                    log("    Hit global network at %s\n", ctx->nameOfWire(cwd.w));
#endif
                    // We've hit the constant pseudo-network, continue from here
                    int cursor2 = cursor;
                    while (cursor2 != src_wire_idx) {
                        auto &c2wd = flat_wires.at(cursor2);
                        bool found = false;
                        for (auto p : ctx->getPipsUphill(c2wd.w)) {
                            if (!ctx->checkPipAvail(p) && ctx->getBoundPipNet(p) != net)
                                continue;
                            WireId src = ctx->getPipSrcWire(p);
                            if (ctx->wireIntent(src) != (const_val ? ID_PSEUDO_VCC : ID_PSEUDO_GND))
                                continue;
                            if (is_wire_undriveable(src))
                                continue;
                            cursor2 = wire_to_idx.at(src);
                            set_visited(t, cursor2, p, WireScore());
                            found = true;
                            break;
                        }
                        if (!found)
                            log_error("Invalid global constant node '%s'\n", ctx->nameOfWire(c2wd.w));
                    }

                    break;
                }
#if 0
                std::string name = ctx->nameOfWire(cwd.w);
                if (name.substr(int(name.size()) - 3) == "A_O") {
                    for (auto uh : ctx->getPipsUphill(flat_wires[cursor].w)) {
                        log("   %s <-- %s %d\n", ctx->nameOfWire(flat_wires[cursor].w), ctx->nameOfWire(ctx->getPipSrcWire(uh)), int(!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net));
                    }
                }
#endif
                bool did_something = false;
                for (auto uh : ctx->getPipsUphill(flat_wires[cursor].w)) {
                    did_something = true;
                    if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                        continue;
                    if (cpip != PipId() && cpip != uh)
                        continue; // don't allow multiple pips driving a wire with a net
                    int next = wire_to_idx.at(ctx->getPipSrcWire(uh));
                    if (was_visited(next))
                        continue; // skip wires that have already been visited
                    auto &wd = flat_wires[next];
                    if (wd.unavailable)
                        continue;
                    if (wd.reserved_net != -1 && wd.reserved_net != net->udata)
                        continue;
                    if (int(wd.bound_nets.size()) > (allowed_cong + 1) ||
                        (allowed_cong == 0 && wd.bound_nets.size() == 1 && !wd.bound_nets.count(net->udata)))
                        continue; // never allow congestion in backwards routing
                    t.backwards_queue.push(next);
                    set_visited(t, next, uh, WireScore());
                }
                if (did_something)
                    ++backwards_iter;
            }
            int dst_wire_idx = wire_to_idx.at(dst_wire);
            if (was_visited(src_wire_idx)) {
                ROUTE_LOG_DBG("   Routed (backwards): ");
                int cursor_fwd = src_wire_idx;
                bind_pip_internal(net, i, src_wire_idx, PipId());
                while (was_visited(cursor_fwd)) {
                    auto &v = flat_wires.at(cursor_fwd).visit;
                    cursor_fwd = wire_to_idx.at(ctx->getPipDstWire(v.pip));
                    bind_pip_internal(net, i, cursor_fwd, v.pip);
                    if (ctx->debug) {
                        auto &wd = flat_wires.at(cursor_fwd);
                        ROUTE_LOG_DBG("      wire: %s (curr %d hist %f)\n", ctx->nameOfWire(wd.w),
                                      int(wd.bound_nets.size()) - 1, wd.hist_cong_cost);
                    }
                }
                NPNR_ASSERT(cursor_fwd == dst_wire_idx);
                ad.routed = true;
                t.processed_sinks.insert(dst_wire);
                reset_wires(t);
                return;
            }
        }
        log_error("Unrouteable %s sink %s.%s (%s)\n", ctx->nameOf(net), ctx->nameOf(net->users.at(i).cell),
                  ctx->nameOf(net->users.at(i).port), ctx->nameOfWire(dst_wire));
    }
#endif

ArcRouteResult Router2State::route_seg(Router2Thread &t, NetInfo *net, size_t i, bool is_mt, bool is_bb)
{

    auto &nd = nets[net->udata];
    auto &sd = nd.segments[i];
    auto &s = sd.s;
    ROUTE_LOG_DBG("Routing segment %d of net '%s' (%d, %d) -> (%d, %d)\n", int(i), ctx->nameOf(net), sd.bb.x0, sd.bb.y0,
                  sd.bb.x1, sd.bb.y1);
    WireId src_wire = s.src_wire, dst_wire = s.dst_wire;
    int src_wire_idx = wire_to_idx.at(src_wire);
    int dst_wire_idx = wire_to_idx.at(dst_wire);
    // Check if arc was already done _in this iteration_
    if (t.processed_sinks.count(dst_wire))
        return ARC_SUCCESS;

    if (!t.queue.empty()) {
        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> new_queue;
        t.queue.swap(new_queue);
    }
    if (!t.backwards_queue.empty()) {
        std::queue<int> new_queue;
        t.backwards_queue.swap(new_queue);
    }
    // First try strongly iteration-limited routing backwards BFS
    // this will deal with certain nets faster than forward A*
    // and comes at a minimal performance cost for the others
    // This could also be used to speed up forwards routing by a hybrid
    // bidirectional approach
    int backwards_iter = 0;
    int backwards_limit =
            ctx->getBelGlobalBuf(net->driver.cell->bel) ? cfg.global_backwards_max_iter : cfg.backwards_max_iter;
    t.backwards_queue.push(wire_to_idx.at(dst_wire));
    while (!t.backwards_queue.empty() && backwards_iter < backwards_limit) {
        int cursor = t.backwards_queue.front();
        t.backwards_queue.pop();
        auto &cwd = flat_wires[cursor];
        PipId cpip;
        if (cwd.bound_nets.count(net->udata)) {
            // If we can tack onto existing routing; try that
            // Only do this if the existing routing is uncontented; however
            int cursor2 = cursor;
            bool bwd_merge_fail = false;
            while (flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                if (flat_wires.at(cursor2).bound_nets.size() > 1) {
                    bwd_merge_fail = true;
                    break;
                }
                PipId p = flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                if (p == PipId())
                    break;
                cursor2 = wire_to_idx.at(ctx->getPipSrcWire(p));
            }
            if (!bwd_merge_fail && cursor2 == src_wire_idx) {
                // Found a path to merge to existing routing; backwards
                cursor2 = cursor;
                while (flat_wires.at(cursor2).bound_nets.count(net->udata)) {
                    PipId p = flat_wires.at(cursor2).bound_nets.at(net->udata).second;
                    if (p == PipId())
                        break;
                    cursor2 = wire_to_idx.at(ctx->getPipSrcWire(p));
                    set_visited(t, cursor2, p, WireScore());
                }
                break;
            }
            cpip = cwd.bound_nets.at(net->udata).second;
        }
        bool did_something = false;
        for (auto uh : ctx->getPipsUphill(flat_wires[cursor].w)) {
            did_something = true;
            if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                continue;
            if (cpip != PipId() && cpip != uh)
                continue; // don't allow multiple pips driving a wire with a net
            int next = wire_to_idx.at(ctx->getPipSrcWire(uh));
            if (was_visited(next))
                continue; // skip wires that have already been visited
            auto &wd = flat_wires[next];
            if (wd.unavailable)
                continue;
            if (wd.reserved_net != -1 && wd.reserved_net != net->udata)
                continue;
            if (wd.bound_nets.size() > 1 || (wd.bound_nets.size() == 1 && !wd.bound_nets.count(net->udata)))
                continue; // never allow congestion in backwards routing
            t.backwards_queue.push(next);
            set_visited(t, next, uh, WireScore());
        }
        if (did_something)
            ++backwards_iter;
    }
    // Check if backwards routing succeeded in reaching source
    if (was_visited(src_wire_idx)) {
        ROUTE_LOG_DBG("   Routed (backwards): ");
        int cursor_fwd = src_wire_idx;
        bind_pip_internal(net, src_wire_idx, PipId());
        while (was_visited(cursor_fwd)) {
            auto &v = flat_wires.at(cursor_fwd).visit;
            cursor_fwd = wire_to_idx.at(ctx->getPipDstWire(v.pip));
            bind_pip_internal(net, cursor_fwd, v.pip);
            if (ctx->debug) {
                auto &wd = flat_wires.at(cursor_fwd);
                ROUTE_LOG_DBG("      wire: %s (curr %d hist %f)\n", ctx->nameOfWire(wd.w),
                              int(wd.bound_nets.size()) - 1, wd.hist_cong_cost);
            }
        }
        NPNR_ASSERT(cursor_fwd == dst_wire_idx);
        sd.routed = true;
        t.processed_sinks.insert(dst_wire);
        reset_wires(t);
        return ARC_SUCCESS;
    }

    // Normal forwards A* routing
    reset_wires(t);
    WireScore base_score;
    base_score.cost = 0;
    base_score.delay = ctx->getWireDelay(src_wire).maxDelay();
    base_score.togo_cost = get_togo_cost(net, src_wire_idx, dst_wire);

    // Add source wire to queue
    t.queue.push(QueuedWire(src_wire_idx, PipId(), Loc(), base_score));
    set_visited(t, src_wire_idx, PipId(), base_score);

    int toexplore = 25000 * std::max(1, (sd.bb.x1 - sd.bb.x0) + (sd.bb.y1 - sd.bb.y0));
    int iter = 0;
    int explored = 1;
    bool debug_arc = /*usr.cell->type.str(ctx).find("RAMB") != std::string::npos && (usr.port ==
                        ctx->id("ADDRATIEHIGH0") || usr.port == ctx->id("ADDRARDADDRL0"))*/
            false;
    while (!t.queue.empty() && (!is_bb || iter < toexplore)) {
        auto curr = t.queue.top();
        auto &d = flat_wires.at(curr.wire);
        t.queue.pop();
        ++iter;
#if 0
            ROUTE_LOG_DBG("current wire %s\n", ctx->nameOfWire(curr.wire));
#endif
        // Explore all pips downhill of cursor
        for (auto dh : ctx->getPipsDownhill(d.w)) {
            // Skip pips outside of box in bounding-box mode
#if 0
                ROUTE_LOG_DBG("trying pip %s\n", ctx->nameOfPip(dh));
#endif
#if 0
                int wire_intent = ctx->wireIntent(curr.wire);
                if (is_bb && !hit_test_pip(ad.bb, ctx->getPipLocation(dh)) && wire_intent != ID_PSEUDO_GND && wire_intent != ID_PSEUDO_VCC)
                    continue;
#else
            if (is_bb && !hit_test_pip(sd.bb, ctx->getPipLocation(dh)))
                continue;
            if (!ctx->checkPipAvail(dh) && ctx->getBoundPipNet(dh) != net)
                continue;
#endif
            // Evaluate score of next wire
            WireId next = ctx->getPipDstWire(dh);
            int next_idx = wire_to_idx.at(next);
            if (was_visited(next_idx))
                continue;
#if 1
            if (debug_arc)
                ROUTE_LOG_DBG("   src wire %s\n", ctx->nameOfWire(next));
#endif
            auto &nwd = flat_wires.at(next_idx);
            if (nwd.unavailable)
                continue;
            if (nwd.reserved_net != -1 && nwd.reserved_net != net->udata)
                continue;
            if (nwd.bound_nets.count(net->udata) && nwd.bound_nets.at(net->udata).second != dh)
                continue;
            WireScore next_score;
            next_score.cost = curr.score.cost + score_wire_for_net(net, next, dh);
            next_score.delay = curr.score.delay + ctx->getPipDelay(dh).maxDelay() + ctx->getWireDelay(next).maxDelay();
            next_score.togo_cost = cfg.estimate_weight * get_togo_cost(net, next_idx, dst_wire);
            const auto &v = nwd.visit;
            if (!v.visited || (v.score.total() > next_score.total())) {
                ++explored;
#if 0
                    ROUTE_LOG_DBG("exploring wire %s cost %f togo %f\n", ctx->nameOfWire(next), next_score.cost,
                                  next_score.togo_cost);
#endif
                // Add wire to queue if it meets criteria
                t.queue.push(QueuedWire(next_idx, dh, ctx->getPipLocation(dh), next_score, ctx->rng()));
                set_visited(t, next_idx, dh, next_score);
                if (next == dst_wire) {
                    toexplore = std::min(toexplore, iter + 5);
                }
            }
        }
    }
    if (was_visited(dst_wire_idx)) {
        ROUTE_LOG_DBG("   Routed (explored %d wires): ", explored);
        int cursor_bwd = dst_wire_idx;
        while (was_visited(cursor_bwd)) {
            auto &v = flat_wires.at(cursor_bwd).visit;
            bind_pip_internal(net, cursor_bwd, v.pip);
            if (ctx->debug) {
                auto &wd = flat_wires.at(cursor_bwd);
                ROUTE_LOG_DBG("      wire: %s (curr %d hist %f share %d)\n", ctx->nameOfWire(wd.w),
                              int(wd.bound_nets.size()) - 1, wd.hist_cong_cost,
                              wd.bound_nets.count(net->udata) ? wd.bound_nets.at(net->udata).first : 0);
            }
            if (v.pip == PipId()) {
                NPNR_ASSERT(cursor_bwd == src_wire_idx);
                break;
            }
            ROUTE_LOG_DBG("         pip: %s (%d, %d)\n", ctx->nameOfPip(v.pip), ctx->getPipLocation(v.pip).x,
                          ctx->getPipLocation(v.pip).y);
            cursor_bwd = wire_to_idx.at(ctx->getPipSrcWire(v.pip));
        }
        t.processed_sinks.insert(dst_wire);
        sd.routed = true;
        reset_wires(t);
        return ARC_SUCCESS;
    } else {
        reset_wires(t);
        return ARC_RETRY_WITHOUT_BB;
    }
}
#undef ARC_ERR

bool Router2State::route_net(Router2Thread &t, NetInfo *net, bool is_mt)
{

#if 0
        if (net->is_global)
            return true;
#endif

    ROUTE_LOG_DBG("Routing net '%s'...\n", ctx->nameOf(net));

    auto rstart = std::chrono::high_resolution_clock::now();

    // Nothing to do if net is undriven
    if (net->driver.cell == nullptr)
        return true;

    bool have_failures = false;
    t.processed_sinks.clear();
    t.route_arcs.clear();
    auto &nd = nets.at(net->udata);
    for (size_t i = 0; i < nd.segments.size(); i++) {
        // Ripup failed arcs to start with
        // Check if arc is already legally routed
        if (check_seg_routing(net, i))
            continue;
        auto &s = nd.segments.at(i).s;
        // Case of arcs that were pre-routed strongly (e.g. clocks)
        if (net->wires.count(s.dst_wire) && net->wires.at(s.dst_wire).strength > STRENGTH_STRONG)
            return ARC_SUCCESS;
        // Ripup arc to start with
        ripup_seg(net, i);
        t.route_arcs.push_back(i);
    }
    for (auto i : t.route_arcs) {
        auto &s = nd.segments.at(i).s;
        auto res1 = route_seg(t, net, i, is_mt, true);
        if (res1 == ARC_FATAL)
            return false; // Arc failed irrecoverably
        else if (res1 == ARC_RETRY_WITHOUT_BB) {
            if (is_mt) {
                // Can't break out of bounding box in multi-threaded mode, so mark this arc as a failure
                have_failures = true;
            } else {
                // Attempt a re-route without the bounding box constraint
                ROUTE_LOG_DBG("Rerouting segment %d of net '%s' without bounding box, possible tricky routing...\n",
                              int(i), ctx->nameOf(net));
                auto res2 = route_seg(t, net, i, is_mt, false);
                // If this also fails, no choice but to give up
                if (res2 != ARC_SUCCESS)
                    log_error("Failed to route segment %d of net '%s', from %s to %s.\n", int(i), ctx->nameOf(net),
                              ctx->nameOfWire(s.src_wire), ctx->nameOfWire(s.dst_wire));
            }
        }
    }
    if (cfg.perf_profile) {
        auto rend = std::chrono::high_resolution_clock::now();
        nets.at(net->udata).total_route_us +=
                (std::chrono::duration_cast<std::chrono::microseconds>(rend - rstart).count());
    }
    return !have_failures;
}
#undef ROUTE_LOG_DBG

void Router2State::update_congestion()
{
    total_overuse = 0;
    overused_wires = 0;
    total_wire_use = 0;
    failed_nets.clear();
    for (auto &wire : flat_wires) {
        total_wire_use += int(wire.bound_nets.size());
        int overuse = int(wire.bound_nets.size()) - 1;
        if (overuse > 0) {
            wire.hist_cong_cost += overuse * hist_cong_weight;
            total_overuse += overuse;
            overused_wires += 1;
            for (auto &bound : wire.bound_nets)
                failed_nets.insert(bound.first);
        }
    }
}

bool Router2State::bind_and_check(NetInfo *net, int seg_idx)
{
#ifdef ARCH_ECP5
    if (net->is_global)
        return true;
#endif
    bool success = true;
    auto &nd = nets.at(net->udata);
    auto &sd = nd.segments.at(seg_idx);

    WireId src = sd.s.src_wire, dst = sd.s.dst_wire;

    // Skip routes where the destination is already bound
    if (dst == WireId() || ctx->getBoundWireNet(dst) == net)
        return true;

    if (dst == src) {
        NetInfo *bound = ctx->getBoundWireNet(src);
        if (bound == nullptr)
            ctx->bindWire(src, net, STRENGTH_WEAK);
        else
            NPNR_ASSERT(bound == net);
        return true;
    }

    // Skip routes where there is no routing (special cases)
    if (!sd.routed)
        return true;

    WireId cursor = dst;

    std::vector<PipId> to_bind;

    while (cursor != src) {
        if (!ctx->checkWireAvail(cursor)) {
            if (ctx->getBoundWireNet(cursor) == net)
                break; // hit the part of the net that is already bound
            else {
                success = false;
                break;
            }
        }
        auto &wd = wire_data(cursor);
        if (!wd.bound_nets.count(net->udata)) {
            log("Failure details:\n");
            log("    Cursor: %s\n", ctx->nameOfWire(cursor));
            log_error("Internal error; incomplete route tree for segment %d of net %s.\n", seg_idx, ctx->nameOf(net));
        }
        auto &p = wd.bound_nets.at(net->udata).second;
        if (!ctx->checkPipAvail(p)) {
            success = false;
            break;
        } else {
            to_bind.push_back(p);
        }
        cursor = ctx->getPipSrcWire(p);
    }

    if (success) {
        if (ctx->getBoundWireNet(src) == nullptr)
            ctx->bindWire(src, net, STRENGTH_WEAK);
        for (auto tb : to_bind)
            ctx->bindPip(tb, net, STRENGTH_WEAK);
    } else {
        ripup_seg(net, seg_idx);
        failed_nets.insert(net->udata);
    }
    return success;
}

bool Router2State::bind_and_check_all()
{
    bool success = true;
    std::vector<WireId> net_wires;
    for (auto net : nets_by_udata) {
#ifdef ARCH_ECP5
        if (net->is_global)
            continue;
#endif
        // Ripup wires and pips used by the net in nextpnr's structures
        net_wires.clear();
        for (auto &w : net->wires) {
            if (w.second.strength <= STRENGTH_STRONG)
                net_wires.push_back(w.first);
        }
        for (auto w : net_wires)
            ctx->unbindWire(w);
        auto &nd = nets.at(net->udata);
        // Bind the arcs using the routes we have discovered
        for (size_t i = 0; i < nd.segments.size(); i++) {
            if (!bind_and_check(net, i)) {
                ++arch_fail;
                success = false;
            }
        }
    }
    return success;
}

void Router2State::write_heatmap(std::ostream &out, bool congestion)
{
    std::vector<std::vector<int>> hm_xy;
    int max_x = 0, max_y = 0;
    for (auto &wd : flat_wires) {
        int val = int(wd.bound_nets.size()) - (congestion ? 1 : 0);
        if (wd.bound_nets.empty())
            continue;
        // Estimate wire location by driving pip location
        PipId drv;
        for (auto &bn : wd.bound_nets)
            if (bn.second.second != PipId()) {
                drv = bn.second.second;
                break;
            }
        if (drv == PipId())
            continue;
        Loc l = ctx->getPipLocation(drv);
        max_x = std::max(max_x, l.x);
        max_y = std::max(max_y, l.y);
        if (l.y >= int(hm_xy.size()))
            hm_xy.resize(l.y + 1);
        if (l.x >= int(hm_xy.at(l.y).size()))
            hm_xy.at(l.y).resize(l.x + 1);
        if (val > 0)
            hm_xy.at(l.y).at(l.x) += val;
    }
    for (int y = 0; y <= max_y; y++) {
        for (int x = 0; x <= max_x; x++) {
            if (y >= int(hm_xy.size()) || x >= int(hm_xy.at(y).size()))
                out << "0,";
            else
                out << hm_xy.at(y).at(x) << ",";
        }
        out << std::endl;
    }
}

void Router2State::partition_nets()
{
    // Create a histogram of positions in X and Y positions
    std::map<int, int> cxs, cys;
    for (auto &n : nets) {
        if (n.cx != -1)
            ++cxs[n.cx];
        if (n.cy != -1)
            ++cys[n.cy];
    }
    // 4-way split for now
    int accum_x = 0, accum_y = 0;
    int halfway = int(nets.size()) / 2;
    for (auto &p : cxs) {
        if (accum_x < halfway && (accum_x + p.second) >= halfway)
            mid_x = p.first;
        accum_x += p.second;
    }
    for (auto &p : cys) {
        if (accum_y < halfway && (accum_y + p.second) >= halfway)
            mid_y = p.first;
        accum_y += p.second;
    }
    if (ctx->verbose) {
        log_info("    x splitpoint: %d\n", mid_x);
        log_info("    y splitpoint: %d\n", mid_y);
    }
    std::vector<int> bins(5, 0);
    for (auto &n : nets) {
        if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
            ++bins[0]; // TL
        else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
            ++bins[1]; // TR
        else if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
            ++bins[2]; // BL
        else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
            ++bins[3]; // BR
        else
            ++bins[4]; // cross-boundary
    }
    if (ctx->verbose)
        for (int i = 0; i < 5; i++)
            log_info("        bin %d N=%d\n", i, bins[i]);
}

void Router2State::router_thread(Router2Thread &t)
{
    for (auto n : t.route_nets) {
        bool result = route_net(t, n, true);
        if (!result)
            t.failed_nets.push_back(n);
    }
}

void Router2State::do_route()
{
    // Don't multithread if fewer than 200 nets (heuristic)
    if (route_queue.size() < 200) {
        Router2Thread st;
        for (size_t j = 0; j < route_queue.size(); j++) {
            route_net(st, nets_by_udata[route_queue[j]], false);
        }
        return;
    }
    const int Nq = 4, Nv = 2, Nh = 2;
    const int N = Nq + Nv + Nh;
    std::vector<Router2Thread> tcs(N + 1);
    for (auto n : route_queue) {
        auto &nd = nets.at(n);
        auto ni = nets_by_udata.at(n);
        int bin = N;
        int le_x = mid_x - cfg.bb_margin_x;
        int rs_x = mid_x + cfg.bb_margin_x;
        int le_y = mid_y - cfg.bb_margin_y;
        int rs_y = mid_y + cfg.bb_margin_y;
        // Quadrants
        if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
            bin = 0;
        else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
            bin = 1;
        else if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
            bin = 2;
        else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
            bin = 3;
        // Vertical split
        else if (nd.bb.y0 < le_y && nd.bb.y1 < le_y)
            bin = Nq + 0;
        else if (nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
            bin = Nq + 1;
        // Horizontal split
        else if (nd.bb.x0 < le_x && nd.bb.x1 < le_x)
            bin = Nq + Nv + 0;
        else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x)
            bin = Nq + Nv + 1;
        tcs.at(bin).route_nets.push_back(ni);
    }
    if (ctx->verbose)
        log_info("%d/%d nets not multi-threadable\n", int(tcs.at(N).route_nets.size()), int(route_queue.size()));
    // Multithreaded part of routing - quadrants
    std::vector<std::thread> threads;
    for (int i = 0; i < Nq; i++) {
        threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
    }
    for (auto &t : threads)
        t.join();
    threads.clear();
    // Vertical splits
    for (int i = Nq; i < Nq + Nv; i++) {
        threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
    }
    for (auto &t : threads)
        t.join();
    threads.clear();
    // Horizontal splits
    for (int i = Nq + Nv; i < Nq + Nv + Nh; i++) {
        threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
    }
    for (auto &t : threads)
        t.join();
    threads.clear();
    // Singlethreaded part of routing - nets that cross partitions
    // or don't fit within bounding box
    for (auto st_net : tcs.at(N).route_nets)
        route_net(tcs.at(N), st_net, false);
    // Failed nets
    for (int i = 0; i < N; i++)
        for (auto fail : tcs.at(i).failed_nets)
            route_net(tcs.at(N), fail, false);
}

void Router2State::operator()()
{
    log_info("Running router2...\n");
    log_info("Setting up routing resources...\n");
    auto rstart = std::chrono::high_resolution_clock::now();
    setup_nets();
    setup_wires();
    find_all_reserved_wires();
    partition_nets();
    curr_cong_weight = cfg.init_curr_cong_weight;
    hist_cong_weight = cfg.hist_cong_weight;
    Router2Thread st;
    int iter = 1;

    for (size_t i = 0; i < nets_by_udata.size(); i++)
        route_queue.push_back(i);

    bool timing_driven = ctx->setting<bool>("timing_driven");
    log_info("Running main router loop...\n");
    do {
        ctx->sorted_shuffle(route_queue);

        if (timing_driven && (int(route_queue.size()) > (int(nets_by_udata.size()) / 50))) {
            // Heuristic: reduce runtime by skipping STA in the case of a "long tail" of a few
            // congested nodes
            get_criticalities(ctx, &net_crit);
            for (auto n : route_queue) {
                IdString name = nets_by_udata.at(n)->name;
                auto fnd = net_crit.find(name);
                auto &net = nets.at(n);
                net.max_crit = 0;
                if (fnd == net_crit.end())
                    continue;
                for (auto c : fnd->second.criticality)
                    net.max_crit = std::max(net.max_crit, c);
            }
            std::stable_sort(route_queue.begin(), route_queue.end(),
                             [&](int na, int nb) { return nets.at(na).max_crit > nets.at(nb).max_crit; });
        }

#if 0
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
                if ((j % 1000) == 0 || j == (route_queue.size() - 1))
                    log("    routed %d/%d\n", int(j), int(route_queue.size()));
            }
#endif
        do_route();
        route_queue.clear();
        update_congestion();
#if 0
            if (iter == 1 && ctx->debug) {
                std::ofstream cong_map("cong_map_0.csv");
                write_heatmap(cong_map, true);
            }
#endif
        if (overused_wires == 0) {
            // Try and actually bind nextpnr Arch API wires
            bind_and_check_all();
        }
        for (auto cn : failed_nets)
            route_queue.push_back(cn);
        log_info("    iter=%d wires=%d overused=%d overuse=%d archfail=%s\n", iter, total_wire_use, overused_wires,
                 total_overuse, overused_wires > 0 ? "NA" : std::to_string(arch_fail).c_str());
        ++iter;
        curr_cong_weight *= cfg.curr_cong_mult;
    } while (!failed_nets.empty());
    if (cfg.perf_profile) {
        std::vector<std::pair<int, IdString>> nets_by_runtime;
        for (auto &n : nets_by_udata) {
            nets_by_runtime.emplace_back(nets.at(n->udata).total_route_us, n->name);
        }
        std::sort(nets_by_runtime.begin(), nets_by_runtime.end(), std::greater<std::pair<int, IdString>>());
        log_info("1000 slowest nets by runtime:\n");
        for (int i = 0; i < std::min(int(nets_by_runtime.size()), 1000); i++) {
            log("        %80s %6d %.1fms\n", nets_by_runtime.at(i).second.c_str(ctx),
                int(ctx->nets.at(nets_by_runtime.at(i).second)->users.size()), nets_by_runtime.at(i).first / 1000.0);
        }
    }
    auto rend = std::chrono::high_resolution_clock::now();
    log_info("Router2 time %.02fs\n", std::chrono::duration<float>(rend - rstart).count());

    log_info("Running router1 to check that route is legal...\n");

    router1(ctx, Router1Cfg(ctx));
}
} // namespace Router2

void router2(Context *ctx, const Router2Cfg &cfg, Router2::Router2ArchFunctions *arch_func)
{
    Router2::Router2State rt(ctx, cfg, arch_func);
    rt.ctx = ctx;
    rt();
}

Router2Cfg::Router2Cfg(Context *ctx)
{
    backwards_max_iter = ctx->setting<int>("router2/bwdMaxIter", 20);
    global_backwards_max_iter = ctx->setting<int>("router2/glbBwdMaxIter", 200);
    bb_margin_x = ctx->setting<int>("router2/bbMargin/x", 3);
    bb_margin_y = ctx->setting<int>("router2/bbMargin/y", 3);
    ipin_cost_adder = ctx->setting<float>("router2/ipinCostAdder", 0.0f);
    bias_cost_factor = ctx->setting<float>("router2/biasCostFactor", 0.25f);
    init_curr_cong_weight = ctx->setting<float>("router2/initCurrCongWeight", 0.5f);
    hist_cong_weight = ctx->setting<float>("router2/histCongWeight", 1.0f);
    curr_cong_mult = ctx->setting<float>("router2/currCongWeightMult", 2.0f);
    estimate_weight = ctx->setting<float>("router2/estimateWeight", 1.75f);
    perf_profile = ctx->setting<float>("router2/perfProfile", false);
}

NEXTPNR_NAMESPACE_END
