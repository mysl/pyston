// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/patchpoints.h"

#include <memory>
#include <unordered_map>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "codegen/compvars.h"
#include "codegen/stackmaps.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"

namespace pyston {

void PatchpointInfo::addFrameVar(const std::string& name, CompilerType* type) {
    frame_vars.push_back(FrameVarInfo({.name = name, .type = type }));
}

int ICSetupInfo::totalSize() const {
    int call_size = CALL_ONLY_SIZE;
    if (getCallingConvention() != llvm::CallingConv::C) {
        // 14 bytes per reg that needs to be spilled
        call_size += 14 * 4;
    }
    return num_slots * slot_size + call_size;
}

static std::vector<PatchpointInfo*> new_patchpoints;

ICSetupInfo* ICSetupInfo::initialize(bool has_return_value, int num_slots, int slot_size, ICType type,
                                     TypeRecorder* type_recorder) {
    ICSetupInfo* rtn = new ICSetupInfo(type, num_slots, slot_size, has_return_value, type_recorder);

    // We use size == CALL_ONLY_SIZE to imply that the call isn't patchable
    assert(rtn->totalSize() > CALL_ONLY_SIZE);

    return rtn;
}

int PatchpointInfo::patchpointSize() {
    if (icinfo) {
        int r = icinfo->totalSize();
        assert(r > CALL_ONLY_SIZE);
        return r;
    }

    return CALL_ONLY_SIZE;
}

bool StackMap::Record::Location::operator==(const StackMap::Record::Location& rhs) {
    // TODO: this check is overly-strict.  Some fields are not used depending
    // on the value of type, and I don't think "flags" is used at all currently.
    return (type == rhs.type) && (flags == rhs.flags) && (regnum == rhs.regnum) && (offset == rhs.offset);
}

void PatchpointInfo::parseLocationMap(StackMap::Record* r, LocationMap* map) {
    assert(r->locations.size() == totalStackmapArgs());

    int cur_arg = frameStackmapArgsStart();

    // printf("parsing pp %ld:\n", reinterpret_cast<int64_t>(this));

    StackMap::Record::Location frame_info_location = r->locations[cur_arg];
    cur_arg++;
    // We could allow the frame_info to exist in a different location for each callsite,
    // but in reality it will always live at a fixed stack offset.
    if (map->frameInfoFound()) {
        assert(frame_info_location == map->frame_info_location);
    } else {
        map->frame_info_location = frame_info_location;
    }

    for (FrameVarInfo& frame_var : frame_vars) {
        int num_args = frame_var.type->numFrameArgs();

        llvm::SmallVector<StackMap::Record::Location, 1> locations;
        locations.append(&r->locations[cur_arg], &r->locations[cur_arg + num_args]);

        // printf("%s %d %d\n", frame_var.name.c_str(), r->locations[cur_arg].type, r->locations[cur_arg].regnum);

        map->names[frame_var.name].locations.push_back(
            LocationMap::LocationTable::LocationEntry({._debug_pp_id = (uint64_t) this,
                                                       .offset = r->offset,
                                                       .length = patchpointSize(),
                                                       .type = frame_var.type,
                                                       .locations = std::move(locations) }));

        cur_arg += num_args;
    }
    assert(cur_arg - frameStackmapArgsStart() == numFrameStackmapArgs());
}

static int extractScratchOffset(PatchpointInfo* pp, StackMap::Record* r) {
    StackMap::Record::Location l = r->locations[pp->scratchStackmapArg()];

    static const int DWARF_RBP_REGNUM = 6;

    assert(l.type == StackMap::Record::Location::LocationType::Direct);
    assert(l.regnum == DWARF_RBP_REGNUM);
    return l.offset;
}

static std::unordered_set<int> extractLiveOuts(StackMap::Record* r, llvm::CallingConv::ID cc) {
    std::unordered_set<int> live_outs;

    // Using the C calling convention, there shouldn't be any non-callee-save registers in here,
    // but LLVM is conservative and will add some.  So with the C cc, ignored the specified live outs
    if (cc != llvm::CallingConv::C) {
        for (const auto& live_out : r->live_outs) {
            live_outs.insert(live_out.regnum);
        }
    }

    // llvm doesn't consider callee-save registers to be live
    // if they're never allocated, but I think it makes much more
    // sense to track them as live_outs.
    // Unfortunately this means we need to be conservative about it unless
    // we can change llvm's behavior.
    live_outs.insert(3);
    live_outs.insert(12);
    live_outs.insert(13);
    live_outs.insert(14);
    live_outs.insert(15);

    return live_outs;
}

void processStackmap(CompiledFunction* cf, StackMap* stackmap) {
    // FIXME: this is pretty hacky, that we don't delete the patchpoints here.
    // We need them currently for the llvm interpreter.
    // Eventually we'll get rid of that and use an AST interpreter, and then we won't need this hack.
    if (cf->effort == EffortLevel::INTERPRETED) {
        assert(!stackmap);
        new_patchpoints.clear();
        return;
    }

    int nrecords = stackmap ? stackmap->records.size() : 0;

    assert(cf->location_map == NULL);
    cf->location_map = new LocationMap();
    if (stackmap)
        cf->location_map->constants = stackmap->constants;

    for (int i = 0; i < nrecords; i++) {
        StackMap::Record* r = stackmap->records[i];

        assert(stackmap->stack_size_records.size() == 1);
        const StackMap::StackSizeRecord& stack_size_record = stackmap->stack_size_records[0];
        int stack_size = stack_size_record.stack_size;

        PatchpointInfo* pp = reinterpret_cast<PatchpointInfo*>(r->id);
        assert(pp);

        if (VERBOSITY()) {
            printf("Processing pp %ld; [%d, %d)\n", reinterpret_cast<int64_t>(pp), r->offset,
                   r->offset + pp->patchpointSize());
        }

        assert(r->locations.size() == pp->totalStackmapArgs());

        int scratch_rbp_offset = extractScratchOffset(pp, r);
        int scratch_size = pp->scratchSize();
        assert(scratch_size % sizeof(void*) == 0);
        assert(scratch_rbp_offset % sizeof(void*) == 0);

        uint8_t* start_addr = (uint8_t*)pp->parentFunction()->code + r->offset;
        uint8_t* end_addr = start_addr + pp->patchpointSize();

        // TODO shouldn't have to do it this way
        void* slowpath_func = extractSlowpathFunc(start_addr);

        //*start_addr = 0xcc;
        // start_addr++;

        int nspills = 0;
        SpillMap frame_remapped;
        // TODO: if we pass the same llvm::Value as the stackmap args, we will get the same reg.
        // we shouldn't then spill that multiple times.
        // we could either deal with it here, or not put the same Value into the patchpoint
        for (int j = pp->frameStackmapArgsStart(), e = j + pp->numFrameStackmapArgs(); j < e; j++) {
            StackMap::Record::Location& l = r->locations[j];

            // updates l, start_addr, scratch_rbp_offset, scratch_size:
            bool spilled = spillFrameArgumentIfNecessary(l, start_addr, end_addr, scratch_rbp_offset, scratch_size,
                                                         frame_remapped);
            if (spilled)
                nspills++;
        }
        ASSERT(nspills <= MAX_FRAME_SPILLS, "did %d spills but expected only %d!", nspills, MAX_FRAME_SPILLS);

        assert(scratch_size % sizeof(void*) == 0);
        assert(scratch_rbp_offset % sizeof(void*) == 0);

        // TODO: is something like this necessary?
        // llvm::sys::Memory::InvalidateInstructionCache(start, getSlotSize());

        pp->parseLocationMap(r, cf->location_map);

        const ICSetupInfo* ic = pp->getICInfo();
        if (ic == NULL) {
            // We have to be using the C calling convention here, so we don't need to check the live outs
            // or save them across the call.
            initializePatchpoint3(slowpath_func, start_addr, end_addr, scratch_rbp_offset, scratch_size,
                                  std::unordered_set<int>(), frame_remapped);
            continue;
        }

        std::unordered_set<int> live_outs(extractLiveOuts(r, ic->getCallingConvention()));

        if (ic->hasReturnValue()) {
            assert(ic->getCallingConvention() == llvm::CallingConv::C
                   || ic->getCallingConvention() == llvm::CallingConv::PreserveAll);

            static const int DWARF_RAX = 0;
            // It's possible that the return value doesn't get used, in which case
            // we can avoid copying back into RAX at the end
            if (live_outs.count(DWARF_RAX)) {
                live_outs.erase(DWARF_RAX);
            }
        }



        auto _p = initializePatchpoint3(slowpath_func, start_addr, end_addr, scratch_rbp_offset, scratch_size,
                                        live_outs, frame_remapped);
        uint8_t* slowpath_start = _p.first;
        uint8_t* slowpath_rtn_addr = _p.second;

        ASSERT(slowpath_start - start_addr >= ic->num_slots * ic->slot_size,
               "Used more slowpath space than expected; change ICSetupInfo::totalSize()?");

        assert(pp->numICStackmapArgs() == 0); // don't do anything with these for now

        std::unique_ptr<ICInfo> icinfo = registerCompiledPatchpoint(
            start_addr, slowpath_start, end_addr, slowpath_rtn_addr, ic,
            StackInfo({ stack_size, scratch_size, scratch_rbp_offset }), std::move(live_outs));

        assert(cf);
        // TODO: unsafe.  hard to use a unique_ptr here though.
        cf->ics.push_back(icinfo.release());
    }

    for (PatchpointInfo* pp : new_patchpoints) {
        const ICSetupInfo* ic = pp->getICInfo();
        if (ic)
            delete ic;
        delete pp;
    }
    new_patchpoints.clear();
}

PatchpointInfo* PatchpointInfo::create(CompiledFunction* parent_cf, const ICSetupInfo* icinfo,
                                       int num_ic_stackmap_args) {
    if (icinfo == NULL)
        assert(num_ic_stackmap_args == 0);

    auto* r = new PatchpointInfo(parent_cf, icinfo, num_ic_stackmap_args);
    new_patchpoints.push_back(r);
    return r;
}

ICSetupInfo* createGenericIC(TypeRecorder* type_recorder, bool has_return_value, int size) {
    return ICSetupInfo::initialize(has_return_value, 1, size, ICSetupInfo::Generic, type_recorder);
}

ICSetupInfo* createGetattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 512, ICSetupInfo::Getattr, type_recorder);
}

ICSetupInfo* createGetitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 512, ICSetupInfo::Getitem, type_recorder);
}

ICSetupInfo* createSetitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 256, ICSetupInfo::Setitem, type_recorder);
}

ICSetupInfo* createDelitemIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 256, ICSetupInfo::Delitem, type_recorder);
}

ICSetupInfo* createSetattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 2, 512, ICSetupInfo::Setattr, type_recorder);
}

ICSetupInfo* createDelattrIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(false, 1, 144, ICSetupInfo::Delattr, type_recorder);
}

ICSetupInfo* createCallsiteIC(TypeRecorder* type_recorder, int num_args) {
    // TODO These are very large, but could probably be made much smaller with IC optimizations
    // - using rewriter2 for better code
    // - not emitting duplicate guards
    return ICSetupInfo::initialize(true, 4, 640 + 48 * num_args, ICSetupInfo::Callsite, type_recorder);
}

ICSetupInfo* createGetGlobalIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 1, 128, ICSetupInfo::GetGlobal, type_recorder);
}

ICSetupInfo* createBinexpIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 4, 512, ICSetupInfo::Binexp, type_recorder);
}

ICSetupInfo* createNonzeroIC(TypeRecorder* type_recorder) {
    return ICSetupInfo::initialize(true, 2, 64, ICSetupInfo::Nonzero, type_recorder);
}

} // namespace pyston
