/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_
#define ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_

#include "escape.h"
#include "nodes.h"
#include "optimization.h"

namespace art {

// A ReferenceInfo contains additional info about a reference such as
// whether it's a singleton, returned, etc.
class ReferenceInfo : public ArenaObject<kArenaAllocMisc> {
 public:
  ReferenceInfo(HInstruction* reference, size_t pos)
      : reference_(reference),
        position_(pos),
        is_singleton_(true),
        is_singleton_and_not_returned_(true),
        is_singleton_and_not_deopt_visible_(true),
        has_index_aliasing_(false) {
    CalculateEscape(reference_,
                    nullptr,
                    &is_singleton_,
                    &is_singleton_and_not_returned_,
                    &is_singleton_and_not_deopt_visible_);
  }

  HInstruction* GetReference() const {
    return reference_;
  }

  size_t GetPosition() const {
    return position_;
  }

  // Returns true if reference_ is the only name that can refer to its value during
  // the lifetime of the method. So it's guaranteed to not have any alias in
  // the method (including its callees).
  bool IsSingleton() const {
    return is_singleton_;
  }

  // Returns true if reference_ is a singleton and not returned to the caller or
  // used as an environment local of an HDeoptimize instruction.
  // The allocation and stores into reference_ may be eliminated for such cases.
  bool IsSingletonAndRemovable() const {
    return is_singleton_and_not_returned_ && is_singleton_and_not_deopt_visible_;
  }

  // Returns true if reference_ is a singleton and returned to the caller or
  // used as an environment local of an HDeoptimize instruction.
  bool IsSingletonAndNonRemovable() const {
    return is_singleton_ &&
           (!is_singleton_and_not_returned_ || !is_singleton_and_not_deopt_visible_);
  }

  bool HasIndexAliasing() {
    return has_index_aliasing_;
  }

  void SetHasIndexAliasing(bool has_index_aliasing) {
    // Only allow setting to true.
    DCHECK(has_index_aliasing);
    has_index_aliasing_ = has_index_aliasing;
  }

 private:
  HInstruction* const reference_;
  const size_t position_;  // position in HeapLocationCollector's ref_info_array_.

  // Can only be referred to by a single name in the method.
  bool is_singleton_;
  // Is singleton and not returned to caller.
  bool is_singleton_and_not_returned_;
  // Is singleton and not used as an environment local of HDeoptimize.
  bool is_singleton_and_not_deopt_visible_;
  // Some heap locations with reference_ have array index aliasing,
  // e.g. arr[i] and arr[j] may be the same location.
  bool has_index_aliasing_;

  DISALLOW_COPY_AND_ASSIGN(ReferenceInfo);
};

// A heap location is a reference-offset/index pair that a value can be loaded from
// or stored to.
class HeapLocation : public ArenaObject<kArenaAllocMisc> {
 public:
  static constexpr size_t kInvalidFieldOffset = -1;

  // TODO: more fine-grained array types.
  static constexpr int16_t kDeclaringClassDefIndexForArrays = -1;

  HeapLocation(ReferenceInfo* ref_info,
               size_t offset,
               HInstruction* index,
               int16_t declaring_class_def_index)
      : ref_info_(ref_info),
        offset_(offset),
        index_(index),
        declaring_class_def_index_(declaring_class_def_index),
        value_killed_by_loop_side_effects_(true) {
    DCHECK(ref_info != nullptr);
    DCHECK((offset == kInvalidFieldOffset && index != nullptr) ||
           (offset != kInvalidFieldOffset && index == nullptr));
    if (ref_info->IsSingleton() && !IsArrayElement()) {
      // Assume this location's value cannot be killed by loop side effects
      // until proven otherwise.
      value_killed_by_loop_side_effects_ = false;
    }
  }

  ReferenceInfo* GetReferenceInfo() const { return ref_info_; }
  size_t GetOffset() const { return offset_; }
  HInstruction* GetIndex() const { return index_; }

  // Returns the definition of declaring class' dex index.
  // It's kDeclaringClassDefIndexForArrays for an array element.
  int16_t GetDeclaringClassDefIndex() const {
    return declaring_class_def_index_;
  }

  bool IsArrayElement() const {
    return index_ != nullptr;
  }

  bool IsValueKilledByLoopSideEffects() const {
    return value_killed_by_loop_side_effects_;
  }

  void SetValueKilledByLoopSideEffects(bool val) {
    value_killed_by_loop_side_effects_ = val;
  }

 private:
  ReferenceInfo* const ref_info_;      // reference for instance/static field or array access.
  const size_t offset_;                // offset of static/instance field.
  HInstruction* const index_;          // index of an array element.
  const int16_t declaring_class_def_index_;  // declaring class's def's dex index.
  bool value_killed_by_loop_side_effects_;   // value of this location may be killed by loop
                                             // side effects because this location is stored
                                             // into inside a loop. This gives
                                             // better info on whether a singleton's location
                                             // value may be killed by loop side effects.

  DISALLOW_COPY_AND_ASSIGN(HeapLocation);
};

// A HeapLocationCollector collects all relevant heap locations and keeps
// an aliasing matrix for all locations.
class HeapLocationCollector : public HGraphVisitor {
 public:
  static constexpr size_t kHeapLocationNotFound = -1;
  // Start with a single uint32_t word. That's enough bits for pair-wise
  // aliasing matrix of 8 heap locations.
  static constexpr uint32_t kInitialAliasingMatrixBitVectorSize = 32;

  explicit HeapLocationCollector(HGraph* graph)
      : HGraphVisitor(graph),
        ref_info_array_(graph->GetArena()->Adapter(kArenaAllocLSE)),
        heap_locations_(graph->GetArena()->Adapter(kArenaAllocLSE)),
        aliasing_matrix_(graph->GetArena(),
                         kInitialAliasingMatrixBitVectorSize,
                         true,
                         kArenaAllocLSE),
        has_heap_stores_(false),
        has_volatile_(false),
        has_monitor_operations_(false) {}

  void CleanUp() {
    heap_locations_.clear();
    ref_info_array_.clear();
  }

  size_t GetNumberOfHeapLocations() const {
    return heap_locations_.size();
  }

  HeapLocation* GetHeapLocation(size_t index) const {
    return heap_locations_[index];
  }

  HInstruction* HuntForOriginalReference(HInstruction* ref) const {
    DCHECK(ref != nullptr);
    while (ref->IsNullCheck() || ref->IsBoundType()) {
      ref = ref->InputAt(0);
    }
    return ref;
  }

  ReferenceInfo* FindReferenceInfoOf(HInstruction* ref) const {
    for (size_t i = 0; i < ref_info_array_.size(); i++) {
      ReferenceInfo* ref_info = ref_info_array_[i];
      if (ref_info->GetReference() == ref) {
        DCHECK_EQ(i, ref_info->GetPosition());
        return ref_info;
      }
    }
    return nullptr;
  }

  size_t GetArrayAccessHeapLocation(HInstruction* array, HInstruction* index) const {
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    HInstruction* original_ref = HuntForOriginalReference(array);
    ReferenceInfo* ref_info = FindReferenceInfoOf(original_ref);
    return FindHeapLocationIndex(ref_info,
                                 HeapLocation::kInvalidFieldOffset,
                                 index,
                                 HeapLocation::kDeclaringClassDefIndexForArrays);
  }

  bool HasHeapStores() const {
    return has_heap_stores_;
  }

  bool HasVolatile() const {
    return has_volatile_;
  }

  bool HasMonitorOps() const {
    return has_monitor_operations_;
  }

  // Find and return the heap location index in heap_locations_.
  size_t FindHeapLocationIndex(ReferenceInfo* ref_info,
                               size_t offset,
                               HInstruction* index,
                               int16_t declaring_class_def_index) const {
    for (size_t i = 0; i < heap_locations_.size(); i++) {
      HeapLocation* loc = heap_locations_[i];
      if (loc->GetReferenceInfo() == ref_info &&
          loc->GetOffset() == offset &&
          loc->GetIndex() == index &&
          loc->GetDeclaringClassDefIndex() == declaring_class_def_index) {
        return i;
      }
    }
    return kHeapLocationNotFound;
  }

  // Returns true if heap_locations_[index1] and heap_locations_[index2] may alias.
  bool MayAlias(size_t index1, size_t index2) const {
    if (index1 < index2) {
      return aliasing_matrix_.IsBitSet(AliasingMatrixPosition(index1, index2));
    } else if (index1 > index2) {
      return aliasing_matrix_.IsBitSet(AliasingMatrixPosition(index2, index1));
    } else {
      DCHECK(false) << "index1 and index2 are expected to be different";
      return true;
    }
  }

  void BuildAliasingMatrix() {
    const size_t number_of_locations = heap_locations_.size();
    if (number_of_locations == 0) {
      return;
    }
    size_t pos = 0;
    // Compute aliasing info between every pair of different heap locations.
    // Save the result in a matrix represented as a BitVector.
    for (size_t i = 0; i < number_of_locations - 1; i++) {
      for (size_t j = i + 1; j < number_of_locations; j++) {
        if (ComputeMayAlias(i, j)) {
          aliasing_matrix_.SetBit(CheckedAliasingMatrixPosition(i, j, pos));
        }
        pos++;
      }
    }
  }

 private:
  // An allocation cannot alias with a name which already exists at the point
  // of the allocation, such as a parameter or a load happening before the allocation.
  bool MayAliasWithPreexistenceChecking(ReferenceInfo* ref_info1, ReferenceInfo* ref_info2) const {
    if (ref_info1->GetReference()->IsNewInstance() || ref_info1->GetReference()->IsNewArray()) {
      // Any reference that can alias with the allocation must appear after it in the block/in
      // the block's successors. In reverse post order, those instructions will be visited after
      // the allocation.
      return ref_info2->GetPosition() >= ref_info1->GetPosition();
    }
    return true;
  }

  bool CanReferencesAlias(ReferenceInfo* ref_info1, ReferenceInfo* ref_info2) const {
    if (ref_info1 == ref_info2) {
      return true;
    } else if (ref_info1->IsSingleton()) {
      return false;
    } else if (ref_info2->IsSingleton()) {
      return false;
    } else if (!MayAliasWithPreexistenceChecking(ref_info1, ref_info2) ||
        !MayAliasWithPreexistenceChecking(ref_info2, ref_info1)) {
      return false;
    }
    return true;
  }

  bool CanArrayIndicesAlias(const HInstruction* i1, const HInstruction* i2) const;

  // `index1` and `index2` are indices in the array of collected heap locations.
  // Returns the position in the bit vector that tracks whether the two heap
  // locations may alias.
  size_t AliasingMatrixPosition(size_t index1, size_t index2) const {
    DCHECK(index2 > index1);
    const size_t number_of_locations = heap_locations_.size();
    // It's (num_of_locations - 1) + ... + (num_of_locations - index1) + (index2 - index1 - 1).
    return (number_of_locations * index1 - (1 + index1) * index1 / 2 + (index2 - index1 - 1));
  }

  // An additional position is passed in to make sure the calculated position is correct.
  size_t CheckedAliasingMatrixPosition(size_t index1, size_t index2, size_t position) {
    size_t calculated_position = AliasingMatrixPosition(index1, index2);
    DCHECK_EQ(calculated_position, position);
    return calculated_position;
  }

  // Compute if two locations may alias to each other.
  bool ComputeMayAlias(size_t index1, size_t index2) const {
    HeapLocation* loc1 = heap_locations_[index1];
    HeapLocation* loc2 = heap_locations_[index2];
    if (loc1->GetOffset() != loc2->GetOffset()) {
      // Either two different instance fields, or one is an instance
      // field and the other is an array element.
      return false;
    }
    if (loc1->GetDeclaringClassDefIndex() != loc2->GetDeclaringClassDefIndex()) {
      // Different types.
      return false;
    }
    if (!CanReferencesAlias(loc1->GetReferenceInfo(), loc2->GetReferenceInfo())) {
      return false;
    }
    if (loc1->IsArrayElement() && loc2->IsArrayElement()) {
      HInstruction* array_index1 = loc1->GetIndex();
      HInstruction* array_index2 = loc2->GetIndex();
      if (!CanArrayIndicesAlias(array_index1, array_index2)) {
        return false;
      }
      ReferenceInfo* ref_info = loc1->GetReferenceInfo();
      ref_info->SetHasIndexAliasing(true);
    }
    return true;
  }

  ReferenceInfo* GetOrCreateReferenceInfo(HInstruction* instruction) {
    ReferenceInfo* ref_info = FindReferenceInfoOf(instruction);
    if (ref_info == nullptr) {
      size_t pos = ref_info_array_.size();
      ref_info = new (GetGraph()->GetArena()) ReferenceInfo(instruction, pos);
      ref_info_array_.push_back(ref_info);
    }
    return ref_info;
  }

  void CreateReferenceInfoForReferenceType(HInstruction* instruction) {
    if (instruction->GetType() != Primitive::kPrimNot) {
      return;
    }
    DCHECK(FindReferenceInfoOf(instruction) == nullptr);
    GetOrCreateReferenceInfo(instruction);
  }

  HeapLocation* GetOrCreateHeapLocation(HInstruction* ref,
                                        size_t offset,
                                        HInstruction* index,
                                        int16_t declaring_class_def_index) {
    HInstruction* original_ref = HuntForOriginalReference(ref);
    ReferenceInfo* ref_info = GetOrCreateReferenceInfo(original_ref);
    size_t heap_location_idx = FindHeapLocationIndex(
        ref_info, offset, index, declaring_class_def_index);
    if (heap_location_idx == kHeapLocationNotFound) {
      HeapLocation* heap_loc = new (GetGraph()->GetArena())
          HeapLocation(ref_info, offset, index, declaring_class_def_index);
      heap_locations_.push_back(heap_loc);
      return heap_loc;
    }
    return heap_locations_[heap_location_idx];
  }

  HeapLocation* VisitFieldAccess(HInstruction* ref, const FieldInfo& field_info) {
    if (field_info.IsVolatile()) {
      has_volatile_ = true;
    }
    const uint16_t declaring_class_def_index = field_info.GetDeclaringClassDefIndex();
    const size_t offset = field_info.GetFieldOffset().SizeValue();
    return GetOrCreateHeapLocation(ref, offset, nullptr, declaring_class_def_index);
  }

  void VisitArrayAccess(HInstruction* array, HInstruction* index) {
    GetOrCreateHeapLocation(array, HeapLocation::kInvalidFieldOffset,
        index, HeapLocation::kDeclaringClassDefIndexForArrays);
  }

  void VisitInstanceFieldGet(HInstanceFieldGet* instruction) OVERRIDE {
    VisitFieldAccess(instruction->InputAt(0), instruction->GetFieldInfo());
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* instruction) OVERRIDE {
    HeapLocation* location = VisitFieldAccess(instruction->InputAt(0), instruction->GetFieldInfo());
    has_heap_stores_ = true;
    if (location->GetReferenceInfo()->IsSingleton()) {
      // A singleton's location value may be killed by loop side effects if it's
      // defined before that loop, and it's stored into inside that loop.
      HLoopInformation* loop_info = instruction->GetBlock()->GetLoopInformation();
      if (loop_info != nullptr) {
        HInstruction* ref = location->GetReferenceInfo()->GetReference();
        DCHECK(ref->IsNewInstance());
        if (loop_info->IsDefinedOutOfTheLoop(ref)) {
          // ref's location value may be killed by this loop's side effects.
          location->SetValueKilledByLoopSideEffects(true);
        } else {
          // ref is defined inside this loop so this loop's side effects cannot
          // kill its location value at the loop header since ref/its location doesn't
          // exist yet at the loop header.
        }
      }
    } else {
      // For non-singletons, value_killed_by_loop_side_effects_ is inited to
      // true.
      DCHECK_EQ(location->IsValueKilledByLoopSideEffects(), true);
    }
  }

  void VisitStaticFieldGet(HStaticFieldGet* instruction) OVERRIDE {
    VisitFieldAccess(instruction->InputAt(0), instruction->GetFieldInfo());
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitStaticFieldSet(HStaticFieldSet* instruction) OVERRIDE {
    VisitFieldAccess(instruction->InputAt(0), instruction->GetFieldInfo());
    has_heap_stores_ = true;
  }

  // We intentionally don't collect HUnresolvedInstanceField/HUnresolvedStaticField accesses
  // since we cannot accurately track the fields.

  void VisitArrayGet(HArrayGet* instruction) OVERRIDE {
    VisitArrayAccess(instruction->InputAt(0), instruction->InputAt(1));
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitArraySet(HArraySet* instruction) OVERRIDE {
    VisitArrayAccess(instruction->InputAt(0), instruction->InputAt(1));
    has_heap_stores_ = true;
  }

  void VisitNewInstance(HNewInstance* new_instance) OVERRIDE {
    // Any references appearing in the ref_info_array_ so far cannot alias with new_instance.
    CreateReferenceInfoForReferenceType(new_instance);
  }

  void VisitNewArray(HNewArray* new_array) OVERRIDE {
    // Any references appearing in the ref_info_array_ so far cannot alias with new_array.
    CreateReferenceInfoForReferenceType(new_array);
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitInvokeVirtual(HInvokeVirtual* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitInvokeInterface(HInvokeInterface* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitInvokeUnresolved(HInvokeUnresolved* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitInvokePolymorphic(HInvokePolymorphic* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitLoadString(HLoadString* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitPhi(HPhi* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitParameterValue(HParameterValue* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitSelect(HSelect* instruction) OVERRIDE {
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitMonitorOperation(HMonitorOperation* monitor ATTRIBUTE_UNUSED) OVERRIDE {
    has_monitor_operations_ = true;
  }

  ArenaVector<ReferenceInfo*> ref_info_array_;   // All references used for heap accesses.
  ArenaVector<HeapLocation*> heap_locations_;    // All heap locations.
  ArenaBitVector aliasing_matrix_;    // aliasing info between each pair of locations.
  bool has_heap_stores_;    // If there is no heap stores, LSE acts as GVN with better
                            // alias analysis and won't be as effective.
  bool has_volatile_;       // If there are volatile field accesses.
  bool has_monitor_operations_;    // If there are monitor operations.

  DISALLOW_COPY_AND_ASSIGN(HeapLocationCollector);
};

class LoadStoreAnalysis : public HOptimization {
 public:
  explicit LoadStoreAnalysis(HGraph* graph)
    : HOptimization(graph, kLoadStoreAnalysisPassName),
      heap_location_collector_(graph) {}

  const HeapLocationCollector& GetHeapLocationCollector() const {
    return heap_location_collector_;
  }

  void Run() OVERRIDE;

  static constexpr const char* kLoadStoreAnalysisPassName = "load_store_analysis";

 private:
  HeapLocationCollector heap_location_collector_;

  DISALLOW_COPY_AND_ASSIGN(LoadStoreAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_
