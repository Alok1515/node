// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/module-instantiate.h"

#include "src/api/api-inl.h"
#include "src/asmjs/asm-js.h"
#include "src/base/atomicops.h"
#include "src/codegen/compiler.h"
#include "src/compiler/wasm-compiler.h"
#include "src/logging/counters-scopes.h"
#include "src/logging/metrics.h"
#include "src/numbers/conversions-inl.h"
#include "src/objects/descriptor-array-inl.h"
#include "src/objects/property-descriptor.h"
#include "src/objects/torque-defined-classes.h"
#include "src/sandbox/trusted-pointer-scope.h"
#include "src/tracing/trace-event.h"
#include "src/utils/utils.h"
#include "src/wasm/code-space-access.h"
#include "src/wasm/compilation-environment-inl.h"
#include "src/wasm/constant-expression-interface.h"
#include "src/wasm/module-compiler.h"
#include "src/wasm/module-decoder-impl.h"
#include "src/wasm/pgo.h"
#include "src/wasm/wasm-code-pointer-table-inl.h"
#include "src/wasm/wasm-constants.h"
#include "src/wasm/wasm-engine.h"
#include "src/wasm/wasm-external-refs.h"
#include "src/wasm/wasm-import-wrapper-cache.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-objects-inl.h"
#include "src/wasm/wasm-opcodes-inl.h"
#include "src/wasm/wasm-subtyping.h"

#ifdef V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS
#include "src/execution/simulator-base.h"
#endif  // V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS

#define TRACE(...)                                          \
  do {                                                      \
    if (v8_flags.trace_wasm_instances) PrintF(__VA_ARGS__); \
  } while (false)

namespace v8::internal::wasm {

namespace {

uint8_t* raw_buffer_ptr(MaybeDirectHandle<JSArrayBuffer> buffer, int offset) {
  return static_cast<uint8_t*>(buffer.ToHandleChecked()->backing_store()) +
         offset;
}

DirectHandle<Map> CreateStructMap(Isolate* isolate,
                                  CanonicalTypeIndex struct_index,
                                  DirectHandle<Map> opt_rtt_parent) {
  const wasm::CanonicalStructType* type =
      wasm::GetTypeCanonicalizer()->LookupStruct(struct_index);
  const int inobject_properties = 0;
  // We have to use the variable size sentinel because the instance size
  // stored directly in a Map is capped at 255 pointer sizes.
  const int map_instance_size = kVariableSizeSentinel;
  const InstanceType instance_type = WASM_STRUCT_TYPE;
  // TODO(jkummerow): If NO_ELEMENTS were supported, we could use that here.
  const ElementsKind elements_kind = TERMINAL_FAST_ELEMENTS_KIND;
  const wasm::CanonicalValueType no_array_element = kWasmBottom;
  constexpr bool shared = false;  // TODO(42204563): Implement.
  // If we had a CanonicalHeapType, we could use that here.
  wasm::CanonicalValueType heaptype = wasm::CanonicalValueType::Ref(
      struct_index, shared, wasm::RefTypeKind::kStruct);
  DirectHandle<WasmTypeInfo> type_info = isolate->factory()->NewWasmTypeInfo(
      heaptype, no_array_element, opt_rtt_parent);
  DirectHandle<Map> map = isolate->factory()->NewContextlessMap(
      instance_type, map_instance_size, elements_kind, inobject_properties);
  map->set_wasm_type_info(*type_info);
  map->SetInstanceDescriptors(isolate,
                              *isolate->factory()->empty_descriptor_array(), 0,
                              SKIP_WRITE_BARRIER);
  map->set_is_extensible(false);
  const int real_instance_size = WasmStruct::Size(type);
  WasmStruct::EncodeInstanceSizeInMap(real_instance_size, *map);
  return map;
}

DirectHandle<Map> CreateArrayMap(Isolate* isolate,
                                 CanonicalTypeIndex array_index,
                                 DirectHandle<Map> opt_rtt_parent) {
  const wasm::CanonicalArrayType* type =
      wasm::GetTypeCanonicalizer()->LookupArray(array_index);
  wasm::CanonicalValueType element_type = type->element_type();
  const int inobject_properties = 0;
  const int instance_size = kVariableSizeSentinel;
  const InstanceType instance_type = WASM_ARRAY_TYPE;
  const ElementsKind elements_kind = TERMINAL_FAST_ELEMENTS_KIND;
  constexpr bool shared = false;  // TODO(42204563): Implement.
  wasm::CanonicalValueType heaptype =
      wasm::CanonicalValueType::Ref(array_index, shared, RefTypeKind::kArray);
  DirectHandle<WasmTypeInfo> type_info = isolate->factory()->NewWasmTypeInfo(
      heaptype, element_type, opt_rtt_parent);
  DirectHandle<Map> map = isolate->factory()->NewContextlessMap(
      instance_type, instance_size, elements_kind, inobject_properties);
  map->set_wasm_type_info(*type_info);
  map->SetInstanceDescriptors(isolate,
                              *isolate->factory()->empty_descriptor_array(), 0,
                              SKIP_WRITE_BARRIER);
  map->set_is_extensible(false);
  WasmArray::EncodeElementSizeInMap(element_type.value_kind_size(), *map);
  return map;
}

}  // namespace

void CreateMapForType(Isolate* isolate, const WasmModule* module,
                      ModuleTypeIndex type_index,
                      DirectHandle<FixedArray> maybe_shared_maps) {
  // Recursive calls for supertypes may already have created this map.
  if (IsMap(maybe_shared_maps->get(type_index.index))) return;

  CanonicalTypeIndex canonical_type_index =
      module->canonical_type_id(type_index);

  // Try to find the canonical map for this type in the isolate store.
  DirectHandle<WeakFixedArray> canonical_rtts =
      direct_handle(isolate->heap()->wasm_canonical_rtts(), isolate);
  DCHECK_GT(static_cast<uint32_t>(canonical_rtts->length()),
            canonical_type_index.index);
  Tagged<MaybeObject> maybe_canonical_map =
      canonical_rtts->get(canonical_type_index.index);
  if (!maybe_canonical_map.IsCleared()) {
    maybe_shared_maps->set(type_index.index,
                           maybe_canonical_map.GetHeapObjectAssumeWeak());
    return;
  }

  DirectHandle<Map> rtt_parent;
  // If the type with {type_index} has an explicit supertype, make sure the
  // map for that supertype is created first, so that the supertypes list
  // that's cached on every RTT can be set up correctly.
  ModuleTypeIndex supertype = module->supertype(type_index);
  if (supertype.valid()) {
    // This recursion is safe, because kV8MaxRttSubtypingDepth limits the
    // number of recursive steps, so we won't overflow the stack.
    CreateMapForType(isolate, module, supertype, maybe_shared_maps);
    // We look up the supertype in {maybe_shared_maps} as a shared type can only
    // inherit from a shared type and vice verca.
    rtt_parent = direct_handle(
        Cast<Map>(maybe_shared_maps->get(supertype.index)), isolate);
  }
  DirectHandle<Map> map;
  switch (module->type(type_index).kind) {
    case TypeDefinition::kStruct:
      map = CreateStructMap(isolate, canonical_type_index, rtt_parent);
      break;
    case TypeDefinition::kArray:
      map = CreateArrayMap(isolate, canonical_type_index, rtt_parent);
      break;
    case TypeDefinition::kFunction:
      map = CreateFuncRefMap(isolate, canonical_type_index, rtt_parent);
      break;
    case TypeDefinition::kCont:
      UNIMPLEMENTED();
  }
  canonical_rtts->set(canonical_type_index.index, MakeWeak(*map));
  maybe_shared_maps->set(type_index.index, *map);
}

namespace {

bool CompareWithNormalizedCType(const CTypeInfo& info,
                                CanonicalValueType expected,
                                CFunctionInfo::Int64Representation int64_rep) {
  MachineType t = MachineType::TypeForCType(info);
  // Wasm representation of bool is i32 instead of i1.
  if (t.semantic() == MachineSemantic::kBool) {
    return expected == kCanonicalI32;
  }
  if (info.GetType() == CTypeInfo::Type::kSeqOneByteString) {
    // WebAssembly does not support one byte strings in fast API calls as
    // runtime type checks are not supported so far.
    return false;
  }

  if (t.representation() == MachineRepresentation::kWord64) {
    if (int64_rep == CFunctionInfo::Int64Representation::kBigInt) {
      return expected == kCanonicalI64;
    }
    DCHECK_EQ(int64_rep, CFunctionInfo::Int64Representation::kNumber);
    return expected == kCanonicalI32 || expected == kCanonicalF32 ||
           expected == kCanonicalF64;
  }
  return t.representation() == expected.machine_representation();
}

enum class ReceiverKind { kFirstParamIsReceiver, kAnyReceiver };

bool IsSupportedWasmFastApiFunction(Isolate* isolate,
                                    const wasm::CanonicalSig* expected_sig,
                                    Tagged<SharedFunctionInfo> shared,
                                    ReceiverKind receiver_kind,
                                    int* out_index) {
  if (!shared->IsApiFunction()) {
    return false;
  }
  if (shared->api_func_data()->GetCFunctionsCount() == 0) {
    return false;
  }
  if (receiver_kind == ReceiverKind::kAnyReceiver &&
      !shared->api_func_data()->accept_any_receiver()) {
    return false;
  }
  if (receiver_kind == ReceiverKind::kAnyReceiver &&
      !IsUndefined(shared->api_func_data()->signature())) {
    // TODO(wasm): CFunctionInfo* signature check.
    return false;
  }

  const auto log_imported_function_mismatch = [&shared, isolate](
                                                  int func_index,
                                                  const char* reason) {
    if (v8_flags.trace_opt) {
      CodeTracer::Scope scope(isolate->GetCodeTracer());
      PrintF(scope.file(), "[disabled optimization for ");
      ShortPrint(*shared, scope.file());
      PrintF(scope.file(),
             " for C function %d, reason: the signature of the imported "
             "function in the Wasm module doesn't match that of the Fast API "
             "function (%s)]\n",
             func_index, reason);
    }
  };

  // C functions only have one return value.
  if (expected_sig->return_count() > 1) {
    // Here and below, we log when the function we call is declared as an Api
    // function but we cannot optimize the call, which might be unexpected. In
    // that case we use the "slow" path making a normal Wasm->JS call and
    // calling the "slow" callback specified in FunctionTemplate::New().
    log_imported_function_mismatch(0, "too many return values");
    return false;
  }

  for (int c_func_id = 0, end = shared->api_func_data()->GetCFunctionsCount();
       c_func_id < end; ++c_func_id) {
    const CFunctionInfo* info =
        shared->api_func_data()->GetCSignature(isolate, c_func_id);
    if (!compiler::IsFastCallSupportedSignature(info)) {
      log_imported_function_mismatch(c_func_id,
                                     "signature not supported by the fast API");
      continue;
    }

    CTypeInfo return_info = info->ReturnInfo();
    // Unsupported if return type doesn't match.
    if (expected_sig->return_count() == 0 &&
        return_info.GetType() != CTypeInfo::Type::kVoid) {
      log_imported_function_mismatch(c_func_id, "too few return values");
      continue;
    }
    // Unsupported if return type doesn't match.
    if (expected_sig->return_count() == 1) {
      if (return_info.GetType() == CTypeInfo::Type::kVoid) {
        log_imported_function_mismatch(c_func_id, "too many return values");
        continue;
      }
      if (!CompareWithNormalizedCType(return_info, expected_sig->GetReturn(0),
                                      info->GetInt64Representation())) {
        log_imported_function_mismatch(c_func_id, "mismatching return value");
        continue;
      }
    }

    if (receiver_kind == ReceiverKind::kFirstParamIsReceiver) {
      if (expected_sig->parameter_count() < 1) {
        log_imported_function_mismatch(
            c_func_id, "at least one parameter is needed as the receiver");
        continue;
      }
      if (!expected_sig->GetParam(0).is_reference()) {
        log_imported_function_mismatch(c_func_id,
                                       "the receiver has to be a reference");
        continue;
      }
    }

    int param_offset =
        receiver_kind == ReceiverKind::kFirstParamIsReceiver ? 1 : 0;
    // Unsupported if arity doesn't match.
    if (expected_sig->parameter_count() - param_offset !=
        info->ArgumentCount() - 1) {
      log_imported_function_mismatch(c_func_id, "mismatched arity");
      continue;
    }
    // Unsupported if any argument types don't match.
    bool param_mismatch = false;
    for (unsigned int i = 0; i < expected_sig->parameter_count() - param_offset;
         ++i) {
      int sig_index = i + param_offset;
      // Arg 0 is the receiver, skip over it since either the receiver does not
      // matter, or we already checked it above.
      CTypeInfo arg = info->ArgumentInfo(i + 1);
      if (!CompareWithNormalizedCType(arg, expected_sig->GetParam(sig_index),
                                      info->GetInt64Representation())) {
        log_imported_function_mismatch(c_func_id, "parameter type mismatch");
        param_mismatch = true;
        break;
      }
      START_ALLOW_USE_DEPRECATED()
      if (arg.GetSequenceType() == CTypeInfo::SequenceType::kIsSequence) {
        log_imported_function_mismatch(c_func_id,
                                       "sequence types are not allowed");
        param_mismatch = true;
        break;
      }
      END_ALLOW_USE_DEPRECATED()
    }
    if (param_mismatch) {
      continue;
    }
    *out_index = c_func_id;
    return true;
  }
  return false;
}

bool ResolveBoundJSFastApiFunction(const wasm::CanonicalSig* expected_sig,
                                   DirectHandle<JSReceiver> callable) {
  Isolate* isolate = Isolate::Current();

  DirectHandle<JSFunction> target;
  if (IsJSBoundFunction(*callable)) {
    auto bound_target = Cast<JSBoundFunction>(callable);
    // Nested bound functions and arguments not supported yet.
    if (bound_target->bound_arguments()->length() > 0) {
      return false;
    }
    if (IsJSBoundFunction(bound_target->bound_target_function())) {
      return false;
    }
    DirectHandle<JSReceiver> bound_target_function(
        bound_target->bound_target_function(), isolate);
    if (!IsJSFunction(*bound_target_function)) {
      return false;
    }
    target = Cast<JSFunction>(bound_target_function);
  } else if (IsJSFunction(*callable)) {
    target = Cast<JSFunction>(callable);
  } else {
    return false;
  }

  DirectHandle<SharedFunctionInfo> shared(target->shared(), isolate);
  int api_function_index = -1;
  // The fast API call wrapper currently does not support function overloading.
  // Therefore, if the matching function is not function 0, the fast API cannot
  // be used.
  return IsSupportedWasmFastApiFunction(isolate, expected_sig, *shared,
                                        ReceiverKind::kAnyReceiver,
                                        &api_function_index) &&
         api_function_index == 0;
}

bool IsStringRef(wasm::CanonicalValueType type) {
  return type.is_abstract_ref() && type.generic_kind() == GenericKind::kString;
}

bool IsExternRef(wasm::CanonicalValueType type) {
  return type.is_abstract_ref() && type.generic_kind() == GenericKind::kExtern;
}

bool IsStringOrExternRef(wasm::CanonicalValueType type) {
  return IsStringRef(type) || IsExternRef(type);
}

bool IsDataViewGetterSig(const wasm::CanonicalSig* sig,
                         wasm::CanonicalValueType return_type) {
  return sig->parameter_count() == 3 && sig->return_count() == 1 &&
         sig->GetParam(0) == wasm::kCanonicalExternRef &&
         sig->GetParam(1) == wasm::kCanonicalI32 &&
         sig->GetParam(2) == wasm::kCanonicalI32 &&
         sig->GetReturn(0) == return_type;
}

bool IsDataViewSetterSig(const wasm::CanonicalSig* sig,
                         wasm::CanonicalValueType value_type) {
  return sig->parameter_count() == 4 && sig->return_count() == 0 &&
         sig->GetParam(0) == wasm::kCanonicalExternRef &&
         sig->GetParam(1) == wasm::kCanonicalI32 &&
         sig->GetParam(2) == value_type &&
         sig->GetParam(3) == wasm::kCanonicalI32;
}

const MachineSignature* GetFunctionSigForFastApiImport(
    Zone* zone, const CFunctionInfo* info) {
  uint32_t arg_count = info->ArgumentCount();
  uint32_t ret_count =
      info->ReturnInfo().GetType() == CTypeInfo::Type::kVoid ? 0 : 1;
  constexpr uint32_t param_offset = 1;

  MachineSignature::Builder sig_builder(zone, ret_count,
                                        arg_count - param_offset);
  if (ret_count) {
    sig_builder.AddReturn(MachineType::TypeForCType(info->ReturnInfo()));
  }

  for (uint32_t i = param_offset; i < arg_count; ++i) {
    sig_builder.AddParam(MachineType::TypeForCType(info->ArgumentInfo(i)));
  }
  return sig_builder.Get();
}

// This detects imports of the forms:
// - `Function.prototype.call.bind(foo)`, where `foo` is something that has a
//   Builtin id.
// - JSFunction with Builtin id (e.g. `parseFloat`, `Math.sin`).
WellKnownImport CheckForWellKnownImport(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data, int func_index,
    DirectHandle<JSReceiver> callable, const wasm::CanonicalSig* sig) {
  WellKnownImport kGeneric = WellKnownImport::kGeneric;  // "using" is C++20.
  if (trusted_instance_data.is_null()) return kGeneric;
  // Check for plain JS functions.
  if (IsJSFunction(*callable)) {
    Tagged<SharedFunctionInfo> sfi = Cast<JSFunction>(*callable)->shared();
    if (!sfi->HasBuiltinId()) return kGeneric;
    // This needs to be a separate switch because it allows other cases than
    // the one below. Merging them would be invalid, because we would then
    // recognize receiver-requiring methods even when they're (erroneously)
    // being imported such that they don't get a receiver.
    switch (sfi->builtin_id()) {
        // =================================================================
        // String-related imports that aren't part of the JS String Builtins
        // proposal.
      case Builtin::kNumberParseFloat:
        if (sig->parameter_count() == 1 && sig->return_count() == 1 &&
            IsStringRef(sig->GetParam(0)) &&
            sig->GetReturn(0) == wasm::kCanonicalF64) {
          return WellKnownImport::kParseFloat;
        }
        break;

        // =================================================================
        // Math functions.
#define COMPARE_MATH_BUILTIN_F64(name)                                       \
  case Builtin::kMath##name: {                                               \
    if (!v8_flags.wasm_math_intrinsics) return kGeneric;                     \
    const FunctionSig* builtin_sig = WasmOpcodes::Signature(kExprF64##name); \
    if (!builtin_sig) {                                                      \
      builtin_sig = WasmOpcodes::AsmjsSignature(kExprF64##name);             \
    }                                                                        \
    DCHECK_NOT_NULL(builtin_sig);                                            \
    if (EquivalentNumericSig(sig, builtin_sig)) {                            \
      return WellKnownImport::kMathF64##name;                                \
    }                                                                        \
    break;                                                                   \
  }

        COMPARE_MATH_BUILTIN_F64(Acos)
        COMPARE_MATH_BUILTIN_F64(Asin)
        COMPARE_MATH_BUILTIN_F64(Atan)
        COMPARE_MATH_BUILTIN_F64(Atan2)
        COMPARE_MATH_BUILTIN_F64(Cos)
        COMPARE_MATH_BUILTIN_F64(Sin)
        COMPARE_MATH_BUILTIN_F64(Tan)
        COMPARE_MATH_BUILTIN_F64(Exp)
        COMPARE_MATH_BUILTIN_F64(Log)
        COMPARE_MATH_BUILTIN_F64(Pow)
        COMPARE_MATH_BUILTIN_F64(Sqrt)

#undef COMPARE_MATH_BUILTIN_F64

      default:
        break;
    }
    return kGeneric;
  }

  // Check for bound JS functions.
  // First part: check that the callable is a bound function whose target
  // is {Function.prototype.call}, and which only binds a receiver.
  if (!IsJSBoundFunction(*callable)) return kGeneric;
  auto bound = Cast<JSBoundFunction>(callable);
  if (bound->bound_arguments()->length() != 0) return kGeneric;
  if (!IsJSFunction(bound->bound_target_function())) return kGeneric;
  Tagged<SharedFunctionInfo> sfi =
      Cast<JSFunction>(bound->bound_target_function())->shared();
  if (!sfi->HasBuiltinId()) return kGeneric;
  if (sfi->builtin_id() != Builtin::kFunctionPrototypeCall) return kGeneric;
  // Second part: check if the bound receiver is one of the builtins for which
  // we have special-cased support.
  Tagged<Object> bound_this = bound->bound_this();
  if (!IsJSFunction(bound_this)) return kGeneric;
  sfi = Cast<JSFunction>(bound_this)->shared();
  Isolate* isolate = Isolate::Current();
  int out_api_function_index = -1;
  if (v8_flags.wasm_fast_api &&
      IsSupportedWasmFastApiFunction(isolate, sig, sfi,
                                     ReceiverKind::kFirstParamIsReceiver,
                                     &out_api_function_index)) {
    Tagged<FunctionTemplateInfo> func_data = sfi->api_func_data();
    NativeModule* native_module = trusted_instance_data->native_module();
    if (!native_module->TrySetFastApiCallTarget(
            func_index,
            func_data->GetCFunction(isolate, out_api_function_index))) {
      return kGeneric;
    }
#ifdef V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS
    Address c_functions[] = {func_data->GetCFunction(isolate, 0)};
    const v8::CFunctionInfo* const c_signatures[] = {
        func_data->GetCSignature(isolate, 0)};
    isolate->simulator_data()->RegisterFunctionsAndSignatures(c_functions,
                                                              c_signatures, 1);
#endif  //  V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS
    // Store the signature of the C++ function in the native_module. We check
    // first if the signature already exists in the native_module such that we
    // do not create a copy of the signature unnecessarily. Since
    // `has_fast_api_signature` and `set_fast_api_signature` don't happen
    // atomically, it is still possible that multiple copies of the signature
    // get created. However, the `TrySetFastApiCallTarget` above guarantees that
    // if there are concurrent calls to `set_cast_api_signature`, then all calls
    // would store the same signature to the native module.
    if (!native_module->has_fast_api_signature(func_index)) {
      native_module->set_fast_api_signature(
          func_index,
          GetFunctionSigForFastApiImport(
              &native_module->module()->signature_zone,
              func_data->GetCSignature(isolate, out_api_function_index)));
    }

    DirectHandle<HeapObject> js_signature(sfi->api_func_data()->signature(),
                                          isolate);
    DirectHandle<Object> callback_data(
        sfi->api_func_data()->callback_data(kAcquireLoad), isolate);
    DirectHandle<WasmFastApiCallData> fast_api_call_data =
        isolate->factory()->NewWasmFastApiCallData(js_signature, callback_data);
    trusted_instance_data->well_known_imports()->set(func_index,
                                                     *fast_api_call_data);
    return WellKnownImport::kFastAPICall;
  }
  if (!sfi->HasBuiltinId()) return kGeneric;
  switch (sfi->builtin_id()) {
#if V8_INTL_SUPPORT
    case Builtin::kStringPrototypeToLocaleLowerCase:
      if (sig->parameter_count() == 2 && sig->return_count() == 1 &&
          IsStringRef(sig->GetParam(0)) && IsStringRef(sig->GetParam(1)) &&
          IsStringRef(sig->GetReturn(0))) {
        DCHECK_GE(func_index, 0);
        trusted_instance_data->well_known_imports()->set(func_index,
                                                         bound_this);
        return WellKnownImport::kStringToLocaleLowerCaseStringref;
      }
      break;
    case Builtin::kStringPrototypeToLowerCaseIntl:
      if (sig->parameter_count() == 1 && sig->return_count() == 1 &&
          IsStringRef(sig->GetParam(0)) && IsStringRef(sig->GetReturn(0))) {
        return WellKnownImport::kStringToLowerCaseStringref;
      } else if (sig->parameter_count() == 1 && sig->return_count() == 1 &&
                 sig->GetParam(0) == wasm::kCanonicalExternRef &&
                 sig->GetReturn(0) == wasm::kCanonicalExternRef) {
        return WellKnownImport::kStringToLowerCaseImported;
      }
      break;
#endif
    case Builtin::kDataViewPrototypeGetBigInt64:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI64)) {
        return WellKnownImport::kDataViewGetBigInt64;
      }
      break;
    case Builtin::kDataViewPrototypeGetBigUint64:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI64)) {
        return WellKnownImport::kDataViewGetBigUint64;
      }
      break;
    case Builtin::kDataViewPrototypeGetFloat32:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalF32)) {
        return WellKnownImport::kDataViewGetFloat32;
      }
      break;
    case Builtin::kDataViewPrototypeGetFloat64:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalF64)) {
        return WellKnownImport::kDataViewGetFloat64;
      }
      break;
    case Builtin::kDataViewPrototypeGetInt8:
      if (sig->parameter_count() == 2 && sig->return_count() == 1 &&
          sig->GetParam(0) == wasm::kCanonicalExternRef &&
          sig->GetParam(1) == wasm::kCanonicalI32 &&
          sig->GetReturn(0) == wasm::kCanonicalI32) {
        return WellKnownImport::kDataViewGetInt8;
      }
      break;
    case Builtin::kDataViewPrototypeGetInt16:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewGetInt16;
      }
      break;
    case Builtin::kDataViewPrototypeGetInt32:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewGetInt32;
      }
      break;
    case Builtin::kDataViewPrototypeGetUint8:
      if (sig->parameter_count() == 2 && sig->return_count() == 1 &&
          sig->GetParam(0) == wasm::kCanonicalExternRef &&
          sig->GetParam(1) == wasm::kCanonicalI32 &&
          sig->GetReturn(0) == wasm::kCanonicalI32) {
        return WellKnownImport::kDataViewGetUint8;
      }
      break;
    case Builtin::kDataViewPrototypeGetUint16:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewGetUint16;
      }
      break;
    case Builtin::kDataViewPrototypeGetUint32:
      if (IsDataViewGetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewGetUint32;
      }
      break;

    case Builtin::kDataViewPrototypeSetBigInt64:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI64)) {
        return WellKnownImport::kDataViewSetBigInt64;
      }
      break;
    case Builtin::kDataViewPrototypeSetBigUint64:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI64)) {
        return WellKnownImport::kDataViewSetBigUint64;
      }
      break;
    case Builtin::kDataViewPrototypeSetFloat32:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalF32)) {
        return WellKnownImport::kDataViewSetFloat32;
      }
      break;
    case Builtin::kDataViewPrototypeSetFloat64:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalF64)) {
        return WellKnownImport::kDataViewSetFloat64;
      }
      break;
    case Builtin::kDataViewPrototypeSetInt8:
      if (sig->parameter_count() == 3 && sig->return_count() == 0 &&
          sig->GetParam(0) == wasm::kCanonicalExternRef &&
          sig->GetParam(1) == wasm::kCanonicalI32 &&
          sig->GetParam(2) == wasm::kCanonicalI32) {
        return WellKnownImport::kDataViewSetInt8;
      }
      break;
    case Builtin::kDataViewPrototypeSetInt16:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewSetInt16;
      }
      break;
    case Builtin::kDataViewPrototypeSetInt32:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewSetInt32;
      }
      break;
    case Builtin::kDataViewPrototypeSetUint8:
      if (sig->parameter_count() == 3 && sig->return_count() == 0 &&
          sig->GetParam(0) == wasm::kCanonicalExternRef &&
          sig->GetParam(1) == wasm::kCanonicalI32 &&
          sig->GetParam(2) == wasm::kCanonicalI32) {
        return WellKnownImport::kDataViewSetUint8;
      }
      break;
    case Builtin::kDataViewPrototypeSetUint16:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewSetUint16;
      }
      break;
    case Builtin::kDataViewPrototypeSetUint32:
      if (IsDataViewSetterSig(sig, wasm::kCanonicalI32)) {
        return WellKnownImport::kDataViewSetUint32;
      }
      break;
    case Builtin::kDataViewPrototypeGetByteLength:
      if (sig->parameter_count() == 1 && sig->return_count() == 1 &&
          sig->GetParam(0) == wasm::kCanonicalExternRef &&
          sig->GetReturn(0) == kCanonicalF64) {
        return WellKnownImport::kDataViewByteLength;
      }
      break;
    case Builtin::kNumberPrototypeToString:
      if (sig->parameter_count() == 2 && sig->return_count() == 1 &&
          sig->GetParam(0) == wasm::kCanonicalI32 &&
          sig->GetParam(1) == wasm::kCanonicalI32 &&
          IsStringOrExternRef(sig->GetReturn(0))) {
        return WellKnownImport::kIntToString;
      }
      if (sig->parameter_count() == 1 && sig->return_count() == 1 &&
          sig->GetParam(0) == wasm::kCanonicalF64 &&
          IsStringOrExternRef(sig->GetReturn(0))) {
        return WellKnownImport::kDoubleToString;
      }
      break;
    case Builtin::kStringPrototypeIndexOf:
      // (string, string, i32) -> (i32).
      if (sig->parameter_count() == 3 && sig->return_count() == 1 &&
          IsStringRef(sig->GetParam(0)) && IsStringRef(sig->GetParam(1)) &&
          sig->GetParam(2) == wasm::kCanonicalI32 &&
          sig->GetReturn(0) == wasm::kCanonicalI32) {
        return WellKnownImport::kStringIndexOf;
      } else if (sig->parameter_count() == 3 && sig->return_count() == 1 &&
                 sig->GetParam(0) == wasm::kCanonicalExternRef &&
                 sig->GetParam(1) == wasm::kCanonicalExternRef &&
                 sig->GetParam(2) == wasm::kCanonicalI32 &&
                 sig->GetReturn(0) == wasm::kCanonicalI32) {
        return WellKnownImport::kStringIndexOfImported;
      }
      break;
    default:
      break;
  }
  return kGeneric;
}

}  // namespace

ResolvedWasmImport::ResolvedWasmImport(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data, int func_index,
    DirectHandle<JSReceiver> callable, const wasm::CanonicalSig* expected_sig,
    CanonicalTypeIndex expected_sig_id, WellKnownImport preknown_import) {
  DCHECK_EQ(expected_sig, wasm::GetTypeCanonicalizer()->LookupFunctionSignature(
                              expected_sig_id));
  SetCallable(Isolate::Current(), callable);
  kind_ = ComputeKind(trusted_instance_data, func_index, expected_sig,
                      expected_sig_id, preknown_import);
}

void ResolvedWasmImport::SetCallable(Isolate* isolate,
                                     Tagged<JSReceiver> callable) {
  SetCallable(isolate, direct_handle(callable, isolate));
}
void ResolvedWasmImport::SetCallable(Isolate* isolate,
                                     DirectHandle<JSReceiver> callable) {
  callable_ = callable;
  trusted_function_data_ = {};
  if (!IsJSFunction(*callable)) return;
  Tagged<SharedFunctionInfo> sfi = Cast<JSFunction>(*callable_)->shared();
  if (sfi->HasWasmFunctionData()) {
    trusted_function_data_ = direct_handle(sfi->wasm_function_data(), isolate);
  }
}

ImportCallKind ResolvedWasmImport::ComputeKind(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data, int func_index,
    const wasm::CanonicalSig* expected_sig, CanonicalTypeIndex expected_sig_id,
    WellKnownImport preknown_import) {
  // If we already have a compile-time import, simply pass that through.
  if (IsCompileTimeImport(preknown_import)) {
    well_known_status_ = preknown_import;
    DCHECK(IsJSFunction(*callable_));
    DCHECK_EQ(Cast<JSFunction>(*callable_)
                  ->shared()
                  ->internal_formal_parameter_count_without_receiver(),
              expected_sig->parameter_count());
    return ImportCallKind::kJSFunctionArityMatch;
  }
  Isolate* isolate = Isolate::Current();
  if (IsWasmSuspendingObject(*callable_)) {
    suspend_ = kSuspend;
    SetCallable(isolate, Cast<WasmSuspendingObject>(*callable_)->callable());
  }
  if (!trusted_function_data_.is_null() &&
      IsWasmExportedFunctionData(*trusted_function_data_)) {
    Tagged<WasmExportedFunctionData> data =
        Cast<WasmExportedFunctionData>(*trusted_function_data_);
    if (!data->MatchesSignature(expected_sig_id)) {
      return ImportCallKind::kLinkError;
    }
    uint32_t function_index = static_cast<uint32_t>(data->function_index());
    if (function_index >=
        data->instance_data()->module()->num_imported_functions) {
      return ImportCallKind::kWasmToWasm;
    }
    // Resolve the shortcut to the underlying callable and continue.
    ImportedFunctionEntry entry(direct_handle(data->instance_data(), isolate),
                                function_index);
    suspend_ = Cast<WasmImportData>(entry.implicit_arg())->suspend();
    SetCallable(isolate, entry.callable());
  }
  if (!trusted_function_data_.is_null() &&
      IsWasmJSFunctionData(*trusted_function_data_)) {
    Tagged<WasmJSFunctionData> js_function_data =
        Cast<WasmJSFunctionData>(*trusted_function_data_);
    suspend_ = js_function_data->GetSuspend();
    if (!js_function_data->MatchesSignature(expected_sig_id)) {
      return ImportCallKind::kLinkError;
    }
    if (IsJSFunction(js_function_data->GetCallable())) {
      Tagged<SharedFunctionInfo> sfi =
          Cast<JSFunction>(js_function_data->GetCallable())->shared();
      if (sfi->HasWasmFunctionData()) {
        // Special case if the underlying callable is a WasmJSFunction or
        // WasmExportedFunction: link the outer WasmJSFunction itself and not
        // the inner callable. Otherwise when the wrapper tiers up, we will try
        // to link the inner WasmJSFunction/WamsExportedFunction which is
        // incorrect.
        return ImportCallKind::kUseCallBuiltin;
      }
    }
    SetCallable(isolate, js_function_data->GetCallable());
  }
  if (WasmCapiFunction::IsWasmCapiFunction(*callable_)) {
    // TODO(jkummerow): Update this to follow the style of the other kinds of
    // functions.
    auto capi_function = Cast<WasmCapiFunction>(callable_);
    if (!capi_function->MatchesSignature(expected_sig_id)) {
      return ImportCallKind::kLinkError;
    }
    return ImportCallKind::kWasmToCapi;
  }
  // Assuming we are calling to JS, check whether this would be a runtime error.
  if (!wasm::IsJSCompatibleSignature(expected_sig)) {
    return ImportCallKind::kRuntimeTypeError;
  }
  // Check if this can be a JS fast API call.
  if (v8_flags.turbo_fast_api_calls &&
      ResolveBoundJSFastApiFunction(expected_sig, callable_)) {
    return ImportCallKind::kWasmToJSFastApi;
  }
  well_known_status_ = CheckForWellKnownImport(
      trusted_instance_data, func_index, callable_, expected_sig);
  if (well_known_status_ == WellKnownImport::kLinkError) {
    return ImportCallKind::kLinkError;
  }
  // TODO(jkummerow): It would be nice to return {kJSFunctionArityMatch} here
  // whenever {well_known_status_ != kGeneric}, so that the generic wrapper
  // can be used instead of a compiled wrapper; but that requires adding
  // support for calling bound functions to the generic wrapper first.

  // For JavaScript calls, determine whether the target has an arity match.
  if (IsJSFunction(*callable_)) {
    auto function = Cast<JSFunction>(callable_);
    DirectHandle<SharedFunctionInfo> shared(function->shared(), isolate);

    if (IsClassConstructor(shared->kind())) {
      // Class constructor will throw anyway.
      return ImportCallKind::kUseCallBuiltin;
    }

    if (shared->internal_formal_parameter_count_without_receiver() ==
        expected_sig->parameter_count()) {
      return ImportCallKind::kJSFunctionArityMatch;
    }

    return ImportCallKind::kJSFunctionArityMismatch;
  }
  // Unknown case. Use the call builtin.
  return ImportCallKind::kUseCallBuiltin;
}

// A helper class to simplify instantiating a module from a module object.
// It closes over the {Isolate}, the {ErrorThrower}, etc.
class InstanceBuilder {
 public:
  InstanceBuilder(Isolate* isolate, v8::metrics::Recorder::ContextId context_id,
                  ErrorThrower* thrower,
                  DirectHandle<WasmModuleObject> module_object,
                  MaybeDirectHandle<JSReceiver> ffi,
                  MaybeDirectHandle<JSArrayBuffer> memory_buffer);

  // Build an instance, in all of its glory.
  MaybeDirectHandle<WasmInstanceObject> Build();
  // Run the start function, if any.
  bool ExecuteStartFunction();

 private:
  Isolate* isolate_;
  v8::metrics::Recorder::ContextId context_id_;
  const WasmEnabledFeatures enabled_;
  const WasmModule* const module_;
  ErrorThrower* thrower_;
  DirectHandle<WasmModuleObject> module_object_;
  MaybeDirectHandle<JSReceiver> ffi_;
  MaybeDirectHandle<JSArrayBuffer> asmjs_memory_buffer_;
  DirectHandle<JSArrayBuffer> untagged_globals_;
  DirectHandle<JSArrayBuffer> shared_untagged_globals_;
  DirectHandle<FixedArray> tagged_globals_;
  DirectHandle<FixedArray> shared_tagged_globals_;
  DirectHandleVector<WasmTagObject> tags_wrappers_;
  DirectHandleVector<WasmTagObject> shared_tags_wrappers_;
  DirectHandle<JSFunction> start_function_;
  DirectHandleVector<Object> sanitized_imports_;
  std::vector<WellKnownImport> well_known_imports_;
  // We pass this {Zone} to the temporary {WasmFullDecoder} we allocate during
  // each call to {EvaluateConstantExpression}, and reset it after each such
  // call. This has been found to improve performance a bit over allocating a
  // new {Zone} each time.
  Zone init_expr_zone_;

  std::string ImportName(uint32_t index) {
    const WasmImport& import = module_->import_table[index];
    const char* wire_bytes_start = reinterpret_cast<const char*>(
        module_object_->native_module()->wire_bytes().data());
    std::ostringstream oss;
    oss << "Import #" << index << " \"";
    oss.write(wire_bytes_start + import.module_name.offset(),
              import.module_name.length());
    oss << "\" \"";
    oss.write(wire_bytes_start + import.field_name.offset(),
              import.field_name.length());
    oss << "\"";
    return oss.str();
  }

  std::string ImportName(uint32_t index, DirectHandle<String> module_name) {
    std::ostringstream oss;
    oss << "Import #" << index << " \"" << module_name->ToCString().get()
        << "\"";
    return oss.str();
  }

  // Look up an import value in the {ffi_} object.
  MaybeDirectHandle<Object> LookupImport(uint32_t index,
                                         DirectHandle<String> module_name,
                                         DirectHandle<String> import_name);

  // Look up an import value in the {ffi_} object specifically for linking an
  // asm.js module. This only performs non-observable lookups, which allows
  // falling back to JavaScript proper (and hence re-executing all lookups) if
  // module instantiation fails.
  MaybeDirectHandle<Object> LookupImportAsm(uint32_t index,
                                            DirectHandle<String> import_name);

  // Load data segments into the memory.
  void LoadDataSegments(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  void WriteGlobalValue(const WasmGlobal& global, const WasmValue& value);

  void SanitizeImports();

  // Creation of a Wasm instance with {Build()} is split into several phases:
  //
  // First phase: initializes (trusted) objects, so if it fails halfway
  // through (when validation errors are encountered), it must not leave
  // pointers to half-initialized objects elsewhere in memory (e.g. it
  // must not register the instance in "uses" lists, nor write active
  // element segments into imported tables).
  MaybeDirectHandle<WasmTrustedInstanceData> Build_Phase1(
      const DisallowJavascriptExecution& no_js);
  // The last part of the first phase finalizes initialization of trusted
  // objects and creates pointers from elsewhere to them. This sub-phase
  // can never fail, but should still happen under the lifetime of the
  // TrustedPointerPublishingScope.
  // When reaching the end of this phase, all created objects (for the
  // instance, tables, globals, etc) must be in fully initialized and
  // self-consistent state, ready to execute user code (such as the "start"
  // function, or fallible user-provided initializers).
  void Build_Phase1_Infallible(
      DirectHandle<WasmTrustedInstanceData> trusted_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_data);
  // Second phase: runs module-provided initializers and as such can fail,
  // but may assume that just-created objects have been initialized to a
  // consistent state, and *must* assume that these objects are already
  // reachable from elsewhere (so must no longer be made inaccessible on
  // failure).
  MaybeDirectHandle<WasmTrustedInstanceData> Build_Phase2(
      DirectHandle<WasmTrustedInstanceData> trusted_data);

  // Allocate the memory.
  MaybeDirectHandle<WasmMemoryObject> AllocateMemory(uint32_t memory_index);

  // Processes a single imported function.
  bool ProcessImportedFunction(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      int import_index, int func_index, DirectHandle<Object> value,
      WellKnownImport preknown_import);

  // Process a single imported table.
  bool ProcessImportedTable(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      int import_index, int table_index, DirectHandle<Object> value);

  // Process a single imported global.
  bool ProcessImportedGlobal(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      int import_index, int global_index, DirectHandle<Object> value);

  // Process a single imported WasmGlobalObject.
  bool ProcessImportedWasmGlobalObject(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      int import_index, const WasmGlobal& global,
      DirectHandle<WasmGlobalObject> global_object);

  // Process the imports, including functions, tables, globals, and memory, in
  // order, loading them from the {ffi_} object. Returns the number of imported
  // functions, or {-1} on error.
  int ProcessImports(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  // Process all imported memories, placing the WasmMemoryObjects in the
  // supplied {FixedArray}.
  bool ProcessImportedMemories(
      DirectHandle<FixedArray> imported_memory_objects);

  template <typename T>
  T* GetRawUntaggedGlobalPtr(const WasmGlobal& global);

  // Process initialization of globals.
  void InitGlobals(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  // Process the exports, creating wrappers for functions, tables, memories,
  // and globals.
  void ProcessExports(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  void SetTableInitialValues(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  void LoadTableSegments(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
      DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data);

  // Creates new tags. Note that some tags might already exist if they were
  // imported, those tags will be reused.
  void InitializeTags(
      DirectHandle<WasmTrustedInstanceData> trusted_instance_data);
};

namespace {
class WriteOutPGOTask : public v8::Task {
 public:
  explicit WriteOutPGOTask(std::weak_ptr<NativeModule> native_module)
      : native_module_(std::move(native_module)) {}

  void Run() final {
    std::shared_ptr<NativeModule> native_module = native_module_.lock();
    if (!native_module) return;
    DumpProfileToFile(native_module->module(), native_module->wire_bytes(),
                      native_module->tiering_budget_array());
    Schedule(std::move(native_module_));
  }

  static void Schedule(std::weak_ptr<NativeModule> native_module) {
    // Write out PGO info every 10 seconds.
    V8::GetCurrentPlatform()->PostDelayedTaskOnWorkerThread(
        TaskPriority::kUserVisible,
        std::make_unique<WriteOutPGOTask>(std::move(native_module)), 10.0);
  }

 private:
  const std::weak_ptr<NativeModule> native_module_;
};

}  // namespace

MaybeDirectHandle<WasmInstanceObject> InstantiateToInstanceObject(
    Isolate* isolate, ErrorThrower* thrower,
    DirectHandle<WasmModuleObject> module_object,
    MaybeDirectHandle<JSReceiver> imports,
    MaybeDirectHandle<JSArrayBuffer> memory_buffer) {
  v8::metrics::Recorder::ContextId context_id =
      isolate->GetOrRegisterRecorderContextId(isolate->native_context());
  InstanceBuilder builder(isolate, context_id, thrower, module_object, imports,
                          memory_buffer);
  MaybeDirectHandle<WasmInstanceObject> instance_object = builder.Build();
  if (!instance_object.is_null()) {
    const std::shared_ptr<NativeModule>& native_module =
        module_object->shared_native_module();
    if (v8_flags.experimental_wasm_pgo_to_file &&
        native_module->ShouldPgoDataBeWritten() &&
        native_module->module()->num_declared_functions > 0) {
      WriteOutPGOTask::Schedule(native_module);
    }
    if (builder.ExecuteStartFunction()) {
      return instance_object;
    }
  }
  DCHECK(isolate->has_exception() || thrower->error());
  return {};
}

InstanceBuilder::InstanceBuilder(
    Isolate* isolate, v8::metrics::Recorder::ContextId context_id,
    ErrorThrower* thrower, DirectHandle<WasmModuleObject> module_object,
    MaybeDirectHandle<JSReceiver> ffi,
    MaybeDirectHandle<JSArrayBuffer> asmjs_memory_buffer)
    : isolate_(isolate),
      context_id_(context_id),
      enabled_(module_object->native_module()->enabled_features()),
      module_(module_object->module()),
      thrower_(thrower),
      module_object_(module_object),
      ffi_(ffi),
      asmjs_memory_buffer_(asmjs_memory_buffer),
      tags_wrappers_(isolate),
      shared_tags_wrappers_(isolate),
      sanitized_imports_(isolate),
      init_expr_zone_(isolate_->allocator(), "constant expression zone") {
  sanitized_imports_.reserve(module_->import_table.size());
  well_known_imports_.reserve(module_->num_imported_functions);
}

// Build an instance, in all of its glory.
MaybeDirectHandle<WasmInstanceObject> InstanceBuilder::Build() {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.wasm.detailed"),
               "wasm.InstanceBuilder.Build");
  // Will check whether {ffi_} is available.
  SanitizeImports();
  if (thrower_->error()) return {};

  // From here on, we expect the build pipeline to run without exiting to JS.
  DisallowJavascriptExecution no_js(isolate_);
  // Start a timer for instantiation time, if we have a high resolution timer.
  base::ElapsedTimer timer;
  if (base::TimeTicks::IsHighResolution()) {
    timer.Start();
  }
  v8::metrics::WasmModuleInstantiated wasm_module_instantiated;

  DirectHandle<WasmTrustedInstanceData> instance_data;
  // Phase 1: uses a {TrustedPointerPublishingScope} to make the new,
  // partially-initialized instance inaccessible in case of failure.
  if (!Build_Phase1(no_js).ToHandle(&instance_data)) return {};
  // Phase 2: assumes that the new instance is already sufficiently
  // consistently initialized to be exposed to user code.
  if (!Build_Phase2(instance_data).ToHandle(&instance_data)) return {};

  wasm_module_instantiated.success = true;
  wasm_module_instantiated.imported_function_count =
      module_->num_imported_functions;
  if (timer.IsStarted()) {
    base::TimeDelta instantiation_time = timer.Elapsed();
    wasm_module_instantiated.wall_clock_duration_in_us =
        instantiation_time.InMicroseconds();
    SELECT_WASM_COUNTER(isolate_->counters(), module_->origin, wasm_instantiate,
                        module_time)
        ->AddTimedSample(instantiation_time);
    isolate_->metrics_recorder()->DelayMainThreadEvent(wasm_module_instantiated,
                                                       context_id_);
  }

  return direct_handle(instance_data->instance_object(), isolate_);
}

MaybeDirectHandle<WasmTrustedInstanceData> InstanceBuilder::Build_Phase1(
    const DisallowJavascriptExecution& no_js) {
  // Any trusted pointers created here will be zapped unless instantiation
  // successfully runs to completion, to prevent trusted objects that violate
  // their own internal invariants because they're only partially-initialized
  // from becoming accessible to untrusted code.
  // We assume failure for now, and will update to success later.
  TrustedPointerPublishingScope publish_trusted_objects(isolate_, no_js);
  publish_trusted_objects.MarkFailure();
  NativeModule* native_module = module_object_->native_module();

  //--------------------------------------------------------------------------
  // Create the WebAssembly.Instance object.
  //--------------------------------------------------------------------------
  TRACE("New module instantiation for %p\n", native_module);
  DirectHandle<WasmTrustedInstanceData> trusted_data =
      WasmTrustedInstanceData::New(isolate_, module_object_, false);
  bool shared = module_object_->module()->has_shared_part;
  DirectHandle<WasmTrustedInstanceData> shared_trusted_data;
  if (shared) {
    shared_trusted_data =
        WasmTrustedInstanceData::New(isolate_, module_object_, true);
    trusted_data->set_shared_part(*shared_trusted_data);
  }

  //--------------------------------------------------------------------------
  // Set up the memory buffers and memory objects and attach them to the
  // instance.
  //--------------------------------------------------------------------------
  if (is_asmjs_module(module_)) {
    CHECK_EQ(1, module_->memories.size());
    DirectHandle<JSArrayBuffer> buffer;
    if (!asmjs_memory_buffer_.ToHandle(&buffer)) {
      // Use an empty JSArrayBuffer for degenerate asm.js modules.
      MaybeDirectHandle<JSArrayBuffer> new_buffer =
          isolate_->factory()->NewJSArrayBufferAndBackingStore(
              0, InitializedFlag::kUninitialized);
      if (!new_buffer.ToHandle(&buffer)) {
        thrower_->RangeError("Out of memory: asm.js memory");
        return {};
      }
      buffer->set_is_detachable(false);
    }
    // asm.js instantiation should have changed the state of the buffer (or we
    // set it above).
    CHECK(!buffer->is_detachable());

    // The maximum number of pages isn't strictly necessary for memory
    // objects used for asm.js, as they are never visible, but we might
    // as well make it accurate.
    auto maximum_pages =
        static_cast<int>(RoundUp(buffer->byte_length(), wasm::kWasmPageSize) /
                         wasm::kWasmPageSize);
    DirectHandle<WasmMemoryObject> memory_object = WasmMemoryObject::New(
        isolate_, buffer, maximum_pages, AddressType::kI32);
    constexpr int kMemoryIndexZero = 0;
    trusted_data->memory_objects()->set(kMemoryIndexZero, *memory_object);
  } else {
    CHECK(asmjs_memory_buffer_.is_null());
    DirectHandle<FixedArray> memory_objects{trusted_data->memory_objects(),
                                            isolate_};
    // First process all imported memories, then allocate non-imported ones.
    if (!ProcessImportedMemories(memory_objects)) {
      return {};
    }
    // Actual Wasm modules can have multiple memories.
    static_assert(kV8MaxWasmMemories <= kMaxUInt32);
    uint32_t num_memories = static_cast<uint32_t>(module_->memories.size());
    for (uint32_t memory_index = 0; memory_index < num_memories;
         ++memory_index) {
      if (!IsUndefined(memory_objects->get(memory_index))) continue;
      DirectHandle<WasmMemoryObject> memory_object;
      if (AllocateMemory(memory_index).ToHandle(&memory_object)) {
        memory_objects->set(memory_index, *memory_object);
      } else {
        DCHECK(isolate_->has_exception() || thrower_->error());
        return {};
      }
    }
  }

  //--------------------------------------------------------------------------
  // Set up the globals for the new instance.
  //--------------------------------------------------------------------------
  uint32_t untagged_globals_buffer_size = module_->untagged_globals_buffer_size;
  if (untagged_globals_buffer_size > 0) {
    MaybeDirectHandle<JSArrayBuffer> result =
        isolate_->factory()->NewJSArrayBufferAndBackingStore(
            untagged_globals_buffer_size, InitializedFlag::kZeroInitialized,
            AllocationType::kOld);

    if (!result.ToHandle(&untagged_globals_)) {
      thrower_->RangeError("Out of memory: wasm globals");
      return {};
    }

    trusted_data->set_untagged_globals_buffer(*untagged_globals_);
    trusted_data->set_globals_start(
        reinterpret_cast<uint8_t*>(untagged_globals_->backing_store()));

    // TODO(14616): Do this only if we have a shared untagged global.
    if (shared) {
      MaybeDirectHandle<JSArrayBuffer> shared_result =
          isolate_->factory()->NewJSArrayBufferAndBackingStore(
              untagged_globals_buffer_size, InitializedFlag::kZeroInitialized,
              AllocationType::kOld);

      if (!shared_result.ToHandle(&shared_untagged_globals_)) {
        thrower_->RangeError("Out of memory: wasm globals");
        return {};
      }

      shared_trusted_data->set_untagged_globals_buffer(
          *shared_untagged_globals_);
      shared_trusted_data->set_globals_start(reinterpret_cast<uint8_t*>(
          shared_untagged_globals_->backing_store()));
    }
  }

  uint32_t tagged_globals_buffer_size = module_->tagged_globals_buffer_size;
  if (tagged_globals_buffer_size > 0) {
    tagged_globals_ = isolate_->factory()->NewFixedArray(
        static_cast<int>(tagged_globals_buffer_size));
    trusted_data->set_tagged_globals_buffer(*tagged_globals_);
    if (shared) {
      shared_tagged_globals_ = isolate_->factory()->NewFixedArray(
          static_cast<int>(tagged_globals_buffer_size));
      shared_trusted_data->set_tagged_globals_buffer(*shared_tagged_globals_);
    }
  }

  //--------------------------------------------------------------------------
  // Set up the array of references to imported globals' array buffers.
  //--------------------------------------------------------------------------
  if (module_->num_imported_mutable_globals > 0) {
    // TODO(binji): This allocates one slot for each mutable global, which is
    // more than required if multiple globals are imported from the same
    // module.
    DirectHandle<FixedArray> buffers_array = isolate_->factory()->NewFixedArray(
        module_->num_imported_mutable_globals, AllocationType::kOld);
    trusted_data->set_imported_mutable_globals_buffers(*buffers_array);
    if (shared) {
      DirectHandle<FixedArray> shared_buffers_array =
          isolate_->factory()->NewFixedArray(
              module_->num_imported_mutable_globals, AllocationType::kOld);
      shared_trusted_data->set_imported_mutable_globals_buffers(
          *shared_buffers_array);
    }
  }

  //--------------------------------------------------------------------------
  // Set up the tag table used for exception tag checks.
  //--------------------------------------------------------------------------
  int tags_count = static_cast<int>(module_->tags.size());
  if (tags_count > 0) {
    DirectHandle<FixedArray> tag_table =
        isolate_->factory()->NewFixedArray(tags_count, AllocationType::kOld);
    trusted_data->set_tags_table(*tag_table);
    tags_wrappers_.resize(tags_count);
    if (shared) {
      DirectHandle<FixedArray> shared_tag_table =
          isolate_->factory()->NewFixedArray(tags_count, AllocationType::kOld);
      shared_trusted_data->set_tags_table(*shared_tag_table);
      shared_tags_wrappers_.resize(tags_count);
    }
  }

  //--------------------------------------------------------------------------
  // Set up table storage space, and initialize it for non-imported tables.
  //--------------------------------------------------------------------------
  int table_count = static_cast<int>(module_->tables.size());
  if (table_count == 0) {
    trusted_data->set_tables(*isolate_->factory()->empty_fixed_array());
    if (shared) {
      shared_trusted_data->set_tables(
          *isolate_->factory()->empty_fixed_array());
    }
  } else {
    DirectHandle<FixedArray> tables =
        isolate_->factory()->NewFixedArray(table_count);
    DirectHandle<ProtectedFixedArray> dispatch_tables =
        isolate_->factory()->NewProtectedFixedArray(table_count);
    trusted_data->set_tables(*tables);
    trusted_data->set_dispatch_tables(*dispatch_tables);
    DirectHandle<FixedArray> shared_tables;
    DirectHandle<ProtectedFixedArray> shared_dispatch_tables;
    if (shared) {
      shared_tables = isolate_->factory()->NewFixedArray(table_count);
      shared_dispatch_tables =
          isolate_->factory()->NewProtectedFixedArray(table_count);
      shared_trusted_data->set_tables(*shared_tables);
      shared_trusted_data->set_dispatch_tables(*shared_dispatch_tables);
    }
    for (int i = module_->num_imported_tables; i < table_count; i++) {
      const WasmTable& table = module_->tables[i];
      CanonicalValueType canonical_type = module_->canonical_type(table.type);
      // Initialize tables with null for now. We will initialize non-defaultable
      // tables later, in {SetTableInitialValues}.
      DirectHandle<WasmDispatchTable> dispatch_table;
      DirectHandle<WasmTableObject> table_obj = WasmTableObject::New(
          isolate_, table.shared ? shared_trusted_data : trusted_data,
          table.type, canonical_type, table.initial_size,
          table.has_maximum_size, table.maximum_size,
          table.type.use_wasm_null()
              ? DirectHandle<HeapObject>{isolate_->factory()->wasm_null()}
              : DirectHandle<HeapObject>{isolate_->factory()->null_value()},
          table.address_type, &dispatch_table);
      (table.shared ? shared_tables : tables)->set(i, *table_obj);
      if (!dispatch_table.is_null()) {
        (table.shared ? shared_dispatch_tables : dispatch_tables)
            ->set(i, *dispatch_table);
        if (i == 0) {
          (table.shared ? shared_trusted_data : trusted_data)
              ->set_dispatch_table0(*dispatch_table);
        }
      }
    }
  }

  //--------------------------------------------------------------------------
  // Process the imports for the module.
  //--------------------------------------------------------------------------
  if (!module_->import_table.empty()) {
    int num_imported_functions =
        ProcessImports(trusted_data, shared_trusted_data);
    if (num_imported_functions < 0) return {};
  }

  //--------------------------------------------------------------------------
  // Create maps for managed objects (GC proposal).
  // Must happen before {InitGlobals} because globals can refer to these maps.
  //--------------------------------------------------------------------------
  if (!module_->isorecursive_canonical_type_ids.empty()) {
    // Make sure all canonical indices have been set.
    DCHECK(module_->MaxCanonicalTypeIndex().valid());
    TypeCanonicalizer::PrepareForCanonicalTypeId(
        isolate_, module_->MaxCanonicalTypeIndex());
  }
  DirectHandle<FixedArray> non_shared_maps = isolate_->factory()->NewFixedArray(
      static_cast<int>(module_->types.size()));
  DirectHandle<FixedArray> shared_maps =
      shared ? isolate_->factory()->NewFixedArray(
                   static_cast<int>(module_->types.size()))
             : DirectHandle<FixedArray>();
  for (uint32_t index = 0; index < module_->types.size(); index++) {
    bool type_is_shared = module_->types[index].is_shared;
    CreateMapForType(isolate_, module_, ModuleTypeIndex{index},
                     type_is_shared ? shared_maps : non_shared_maps);
  }
  trusted_data->set_managed_object_maps(*non_shared_maps);
  if (shared) shared_trusted_data->set_managed_object_maps(*shared_maps);
#if DEBUG
  for (uint32_t i = 0; i < module_->types.size(); i++) {
    DirectHandle<FixedArray> maps =
        module_->types[i].is_shared ? shared_maps : non_shared_maps;
    Tagged<Object> o = maps->get(i);
    DCHECK(IsMap(o));
    Tagged<Map> map = Cast<Map>(o);
    ModuleTypeIndex index{i};
    if (module_->has_signature(index)) {
      DCHECK_EQ(map->instance_type(), WASM_FUNC_REF_TYPE);
    } else if (module_->has_array(index)) {
      DCHECK_EQ(map->instance_type(), WASM_ARRAY_TYPE);
    } else if (module_->has_struct(index)) {
      DCHECK_EQ(map->instance_type(), WASM_STRUCT_TYPE);
    }
  }
#endif

  //--------------------------------------------------------------------------
  // Allocate the array that will hold type feedback vectors.
  //--------------------------------------------------------------------------
  if (v8_flags.wasm_inlining) {
    int num_functions = static_cast<int>(module_->num_declared_functions);
    // Zero-fill the array so we can do a quick Smi-check to test if a given
    // slot was initialized.
    DirectHandle<FixedArray> vectors =
        isolate_->factory()->NewFixedArrayWithZeroes(num_functions,
                                                     AllocationType::kOld);
    trusted_data->set_feedback_vectors(*vectors);
    if (shared) {
      DirectHandle<FixedArray> shared_vectors =
          isolate_->factory()->NewFixedArrayWithZeroes(num_functions,
                                                       AllocationType::kOld);
      shared_trusted_data->set_feedback_vectors(*shared_vectors);
    }
  }

  //--------------------------------------------------------------------------
  // Process the initialization for the module's globals.
  //--------------------------------------------------------------------------
  InitGlobals(trusted_data, shared_trusted_data);

  //--------------------------------------------------------------------------
  // Initialize non-defaultable tables.
  //--------------------------------------------------------------------------
  SetTableInitialValues(trusted_data, shared_trusted_data);

  //--------------------------------------------------------------------------
  // Initialize the tags table.
  //--------------------------------------------------------------------------
  if (tags_count > 0) {
    InitializeTags(trusted_data);
  }

  //--------------------------------------------------------------------------
  // Set up the exports object for the new instance.
  //--------------------------------------------------------------------------
  ProcessExports(trusted_data, shared_trusted_data);
  if (thrower_->error()) return {};

  //--------------------------------------------------------------------------
  // Set up uninitialized element segments.
  //--------------------------------------------------------------------------
  if (!module_->elem_segments.empty()) {
    DirectHandle<FixedArray> elements = isolate_->factory()->NewFixedArray(
        static_cast<int>(module_->elem_segments.size()));
    DirectHandle<FixedArray> shared_elements =
        shared ? isolate_->factory()->NewFixedArray(
                     static_cast<int>(module_->elem_segments.size()))
               : DirectHandle<FixedArray>();
    for (uint32_t i = 0; i < module_->elem_segments.size(); i++) {
      // Initialize declarative segments as empty. The rest remain
      // uninitialized.
      bool is_declarative = module_->elem_segments[i].status ==
                            WasmElemSegment::kStatusDeclarative;
      (module_->elem_segments[i].shared ? shared_elements : elements)
          ->set(i, is_declarative
                       ? Cast<Object>(*isolate_->factory()->empty_fixed_array())
                       : *isolate_->factory()->undefined_value());
    }
    trusted_data->set_element_segments(*elements);
    if (shared) shared_trusted_data->set_element_segments(*shared_elements);
  }

  //--------------------------------------------------------------------------
  // Create a wrapper for the start function.
  //--------------------------------------------------------------------------
  if (module_->start_function_index >= 0) {
    int start_index = module_->start_function_index;
    auto& function = module_->functions[start_index];

    DCHECK(start_function_.is_null());
    if (function.imported) {
      ImportedFunctionEntry entry(trusted_data, module_->start_function_index);
      Tagged<Object> callable = entry.maybe_callable();
      if (IsJSFunction(callable)) {
        // If the start function was imported and calls into Blink, we have
        // to pretend that the V8 API was used to enter its correct context.
        // In order to simplify entering the context in {ExecuteStartFunction}
        // below, we just record the callable as the start function.
        start_function_ = direct_handle(Cast<JSFunction>(callable), isolate_);
      }
    }
    if (start_function_.is_null()) {
      // TODO(clemensb): Don't generate an exported function for the start
      // function. Use CWasmEntry instead.
      bool function_is_shared = module_->type(function.sig_index).is_shared;
      DirectHandle<WasmFuncRef> func_ref =
          WasmTrustedInstanceData::GetOrCreateFuncRef(
              isolate_, function_is_shared ? shared_trusted_data : trusted_data,
              start_index);
      DirectHandle<WasmInternalFunction> internal{func_ref->internal(isolate_),
                                                  isolate_};
      start_function_ = WasmInternalFunction::GetOrCreateExternal(internal);
    }
  }

  DCHECK(!isolate_->has_exception());
  TRACE("Successfully built instance for module %p\n",
        module_object_->native_module());

#if V8_ENABLE_DRUMBRAKE
  // Skip this event because not (yet) supported by Chromium.

  // v8::metrics::WasmInterpreterJitStatus jit_status;
  // jit_status.jitless = v8_flags.wasm_jitless;
  // isolate_->metrics_recorder()->DelayMainThreadEvent(jit_status,
  // context_id_);
#endif  // V8_ENABLE_DRUMBRAKE

  publish_trusted_objects.MarkSuccess();
  Build_Phase1_Infallible(trusted_data, shared_trusted_data);
  return trusted_data;
}

void InstanceBuilder::Build_Phase1_Infallible(
    DirectHandle<WasmTrustedInstanceData> trusted_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_data) {
  //--------------------------------------------------------------------------
  // Register with memories.
  //--------------------------------------------------------------------------
  size_t num_memories = module_->memories.size();
  DirectHandle<FixedArray> memory_objects{trusted_data->memory_objects(),
                                          isolate_};
  for (uint32_t i = 0; i < num_memories; i++) {
    DirectHandle<WasmMemoryObject> memory{
        Cast<WasmMemoryObject>(memory_objects->get(i)), isolate_};
    WasmMemoryObject::UseInInstance(isolate_, memory, trusted_data,
                                    shared_trusted_data, i);
  }

  //--------------------------------------------------------------------------
  // Register with tables.
  //--------------------------------------------------------------------------
  size_t num_tables = module_->tables.size();
  for (uint32_t i = 0; i < num_tables; i++) {
    const WasmTable& table = module_->tables[i];
    DirectHandle<WasmTrustedInstanceData> data_part =
        table.shared ? shared_trusted_data : trusted_data;
    Tagged<Object> maybe_dispatch_table = data_part->dispatch_tables()->get(i);
    if (maybe_dispatch_table == Smi::zero()) continue;  // Not a function table.
    DirectHandle<WasmDispatchTable> dispatch_table{
        Cast<WasmDispatchTable>(maybe_dispatch_table), isolate_};
    WasmDispatchTable::AddUse(isolate_, dispatch_table, data_part, i);
  }
}

MaybeDirectHandle<WasmTrustedInstanceData> InstanceBuilder::Build_Phase2(
    DirectHandle<WasmTrustedInstanceData> trusted_data) {
  DirectHandle<WasmTrustedInstanceData> shared_trusted_data;
  if (module_->has_shared_part) {
    shared_trusted_data = direct_handle(trusted_data->shared_part(), isolate_);
  }

  //--------------------------------------------------------------------------
  // Load element segments into tables.
  //--------------------------------------------------------------------------
  if (module_->tables.size() > 0) {
    LoadTableSegments(trusted_data, shared_trusted_data);
    if (thrower_->error()) return {};
  }

  //--------------------------------------------------------------------------
  // Initialize the memory by loading data segments.
  //--------------------------------------------------------------------------
  if (!module_->data_segments.empty()) {
    LoadDataSegments(trusted_data, shared_trusted_data);
    if (thrower_->error()) return {};
  }

  return trusted_data;
}

bool InstanceBuilder::ExecuteStartFunction() {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.wasm.detailed"),
               "wasm.ExecuteStartFunction");
  if (start_function_.is_null()) return true;  // No start function.

  HandleScope scope(isolate_);
  // In case the start function calls out to Blink, we have to make sure that
  // the correct "entered context" is available. This is the equivalent of
  // v8::Context::Enter() and must happen in addition to the function call
  // sequence doing the compiled version of "isolate->set_context(...)".
  HandleScopeImplementer* hsi = isolate_->handle_scope_implementer();
  hsi->EnterContext(start_function_->native_context());

  // Call the JS function.
  DirectHandle<Object> undefined = isolate_->factory()->undefined_value();
  MaybeDirectHandle<Object> retval =
      Execution::Call(isolate_, start_function_, undefined, {});
  hsi->LeaveContext();
  // {start_function_} has to be called only once.
  start_function_ = {};

  if (retval.is_null()) {
    DCHECK(isolate_->has_exception());
    return false;
  }
  return true;
}

// Look up an import value in the {ffi_} object.
MaybeDirectHandle<Object> InstanceBuilder::LookupImport(
    uint32_t index, DirectHandle<String> module_name,
    DirectHandle<String> import_name) {
  // The caller checked that the ffi object is present; and we checked in
  // the JS-API layer that the ffi object, if present, is a JSObject.
  DCHECK(!ffi_.is_null());
  // Look up the module first.
  DirectHandle<Object> module;
  DirectHandle<JSReceiver> module_recv;
  if (!Object::GetPropertyOrElement(isolate_, ffi_.ToHandleChecked(),
                                    module_name)
           .ToHandle(&module) ||
      !TryCast<JSReceiver>(module, &module_recv)) {
    const char* error = module.is_null()
                            ? "module not found"
                            : "module is not an object or function";
    thrower_->TypeError("%s: %s", ImportName(index, module_name).c_str(),
                        error);
    return {};
  }

  MaybeDirectHandle<Object> value =
      Object::GetPropertyOrElement(isolate_, module_recv, import_name);
  if (value.is_null()) {
    thrower_->LinkError("%s: import not found", ImportName(index).c_str());
    return {};
  }

  return value;
}

namespace {
bool HasDefaultToNumberBehaviour(Isolate* isolate,
                                 DirectHandle<JSFunction> function) {
  // Disallow providing a [Symbol.toPrimitive] member.
  LookupIterator to_primitive_it{isolate, function,
                                 isolate->factory()->to_primitive_symbol()};
  if (to_primitive_it.state() != LookupIterator::NOT_FOUND) return false;

  // The {valueOf} member must be the default "ObjectPrototypeValueOf".
  LookupIterator value_of_it{isolate, function,
                             isolate->factory()->valueOf_string()};
  if (value_of_it.state() != LookupIterator::DATA) return false;
  DirectHandle<Object> value_of = value_of_it.GetDataValue();
  if (!IsJSFunction(*value_of)) return false;
  Builtin value_of_builtin_id =
      Cast<JSFunction>(value_of)->code(isolate)->builtin_id();
  if (value_of_builtin_id != Builtin::kObjectPrototypeValueOf) return false;

  // The {toString} member must be the default "FunctionPrototypeToString".
  LookupIterator to_string_it{isolate, function,
                              isolate->factory()->toString_string()};
  if (to_string_it.state() != LookupIterator::DATA) return false;
  DirectHandle<Object> to_string = to_string_it.GetDataValue();
  if (!IsJSFunction(*to_string)) return false;
  Builtin to_string_builtin_id =
      Cast<JSFunction>(to_string)->code(isolate)->builtin_id();
  if (to_string_builtin_id != Builtin::kFunctionPrototypeToString) return false;

  // Just a default function, which will convert to "Nan". Accept this.
  return true;
}

bool MaybeMarkError(ValueOrError value, ErrorThrower* thrower) {
  if (is_error(value)) {
    thrower->RuntimeError("%s",
                          MessageFormatter::TemplateString(to_error(value)));
    return true;
  }
  return false;
}
}  // namespace

// Look up an import value in the {ffi_} object specifically for linking an
// asm.js module. This only performs non-observable lookups, which allows
// falling back to JavaScript proper (and hence re-executing all lookups) if
// module instantiation fails.
MaybeDirectHandle<Object> InstanceBuilder::LookupImportAsm(
    uint32_t index, DirectHandle<String> import_name) {
  // The caller checked that the ffi object is present.
  DCHECK(!ffi_.is_null());

  // Perform lookup of the given {import_name} without causing any observable
  // side-effect. We only accept accesses that resolve to data properties,
  // which is indicated by the asm.js spec in section 7 ("Linking") as well.
  PropertyKey key(isolate_, Cast<Name>(import_name));
  LookupIterator it(isolate_, ffi_.ToHandleChecked(), key);
  switch (it.state()) {
    case LookupIterator::ACCESS_CHECK:
    case LookupIterator::TYPED_ARRAY_INDEX_NOT_FOUND:
    case LookupIterator::INTERCEPTOR:
    case LookupIterator::JSPROXY:
    case LookupIterator::WASM_OBJECT:
    case LookupIterator::ACCESSOR:
    case LookupIterator::TRANSITION:
      thrower_->LinkError("%s: not a data property",
                          ImportName(index, import_name).c_str());
      return {};
    case LookupIterator::NOT_FOUND:
      // Accepting missing properties as undefined does not cause any
      // observable difference from JavaScript semantics, we are lenient.
      return isolate_->factory()->undefined_value();
    case LookupIterator::DATA: {
      DirectHandle<Object> value = it.GetDataValue();
      // For legacy reasons, we accept functions for imported globals (see
      // {ProcessImportedGlobal}), but only if we can easily determine that
      // their Number-conversion is side effect free and returns NaN (which is
      // the case as long as "valueOf" (or others) are not overwritten).
      if (IsJSFunction(*value) &&
          module_->import_table[index].kind == kExternalGlobal &&
          !HasDefaultToNumberBehaviour(isolate_, Cast<JSFunction>(value))) {
        thrower_->LinkError("%s: function has special ToNumber behaviour",
                            ImportName(index, import_name).c_str());
        return {};
      }
      return value;
    }
  }
}

// Load data segments into the memory.
// TODO(14616): Consider what to do with shared memories.
void InstanceBuilder::LoadDataSegments(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  base::Vector<const uint8_t> wire_bytes =
      module_object_->native_module()->wire_bytes();
  for (const WasmDataSegment& segment : module_->data_segments) {
    uint32_t size = segment.source.length();

    // Passive segments are not copied during instantiation.
    if (!segment.active) continue;

    const WasmMemory& dst_memory = module_->memories[segment.memory_index];
    size_t dest_offset;
    ValueOrError result = EvaluateConstantExpression(
        &init_expr_zone_, segment.dest_addr,
        dst_memory.is_memory64() ? kWasmI64 : kWasmI32, module_, isolate_,
        trusted_instance_data, shared_trusted_instance_data);
    if (MaybeMarkError(result, thrower_)) return;
    if (dst_memory.is_memory64()) {
      uint64_t dest_offset_64 = to_value(result).to_u64();

      // Clamp to {std::numeric_limits<size_t>::max()}, which is always an
      // invalid offset, so we always fail the bounds check below.
      DCHECK_GT(std::numeric_limits<size_t>::max(), dst_memory.max_memory_size);
      dest_offset = static_cast<size_t>(std::min(
          dest_offset_64, uint64_t{std::numeric_limits<size_t>::max()}));
    } else {
      dest_offset = to_value(result).to_u32();
    }

    size_t memory_size =
        trusted_instance_data->memory_size(segment.memory_index);
    if (!base::IsInBounds<size_t>(dest_offset, size, memory_size)) {
      size_t segment_index = &segment - module_->data_segments.data();
      thrower_->RuntimeError(
          "data segment %zu is out of bounds (offset %zu, "
          "length %u, memory size %zu)",
          segment_index, dest_offset, size, memory_size);
      return;
    }

    uint8_t* memory_base =
        trusted_instance_data->memory_base(segment.memory_index);
    std::memcpy(memory_base + dest_offset,
                wire_bytes.begin() + segment.source.offset(), size);
  }
}

void InstanceBuilder::WriteGlobalValue(const WasmGlobal& global,
                                       const WasmValue& value) {
  TRACE("init [globals_start=%p + %u] = %s, type = %s\n",
        global.type.is_reference()
            ? reinterpret_cast<uint8_t*>(tagged_globals_->address())
            : raw_buffer_ptr(untagged_globals_, 0),
        global.offset, value.to_string().c_str(), global.type.name().c_str());
  DCHECK(global.mutability
             ? (value.type() == module_->canonical_type(global.type))
             : IsSubtypeOf(value.type(), module_->canonical_type(global.type)));
  if (global.type.is_numeric()) {
    value.CopyTo(GetRawUntaggedGlobalPtr<uint8_t>(global));
  } else {
    tagged_globals_->set(global.offset, *value.to_ref());
  }
}

// Returns the name, Builtin ID, and "length" (in the JSFunction sense, i.e.
// number of parameters) for the function representing the given import.
std::tuple<const char*, Builtin, int> NameBuiltinLength(WellKnownImport wki) {
#define CASE(CamelName, name, length)       \
  case WellKnownImport::kString##CamelName: \
    return std::make_tuple(name, Builtin::kWebAssemblyString##CamelName, length)
  switch (wki) {
    CASE(Cast, "cast", 1);
    CASE(CharCodeAt, "charCodeAt", 2);
    CASE(CodePointAt, "codePointAt", 2);
    CASE(Compare, "compare", 2);
    CASE(Concat, "concat", 2);
    CASE(Equals, "equals", 2);
    CASE(FromCharCode, "fromCharCode", 1);
    CASE(FromCodePoint, "fromCodePoint", 1);
    CASE(FromUtf8Array, "decodeStringFromUTF8Array", 3);
    CASE(FromWtf16Array, "fromCharCodeArray", 3);
    CASE(IntoUtf8Array, "encodeStringIntoUTF8Array", 3);
    CASE(Length, "length", 1);
    CASE(MeasureUtf8, "measureStringAsUTF8", 1);
    CASE(Substring, "substring", 3);
    CASE(Test, "test", 1);
    CASE(ToUtf8Array, "encodeStringToUTF8Array", 1);
    CASE(ToWtf16Array, "intoCharCodeArray", 3);
    default:
      UNREACHABLE();  // Only call this for compile-time imports.
  }
#undef CASE
}

DirectHandle<JSFunction> CreateFunctionForCompileTimeImport(
    Isolate* isolate, WellKnownImport wki) {
  auto [name, builtin, length] = NameBuiltinLength(wki);
  Factory* factory = isolate->factory();
  DirectHandle<NativeContext> context(isolate->native_context());
  DirectHandle<Map> map = isolate->strict_function_without_prototype_map();
  DirectHandle<String> name_str = factory->InternalizeUtf8String(name);
  DirectHandle<SharedFunctionInfo> info =
      factory->NewSharedFunctionInfoForBuiltin(name_str, builtin, length,
                                               kAdapt);
  info->set_native(true);
  info->set_language_mode(LanguageMode::kStrict);
  DirectHandle<JSFunction> fun =
      Factory::JSFunctionBuilder{isolate, info, context}.set_map(map).Build();
  return fun;
}

void InstanceBuilder::SanitizeImports() {
  NativeModule* native_module = module_object_->native_module();
  base::Vector<const uint8_t> wire_bytes = native_module->wire_bytes();
  const WellKnownImportsList& well_known_imports =
      module_->type_feedback.well_known_imports;
  const std::string& magic_string_constants =
      native_module->compile_imports().constants_module();
  const bool has_magic_string_constants =
      native_module->compile_imports().contains(
          CompileTimeImport::kStringConstants);
  for (uint32_t index = 0; index < module_->import_table.size(); ++index) {
    const WasmImport& import = module_->import_table[index];

    if (import.kind == kExternalGlobal && has_magic_string_constants &&
        import.module_name.length() == magic_string_constants.size() &&
        std::equal(magic_string_constants.begin(), magic_string_constants.end(),
                   wire_bytes.begin() + import.module_name.offset())) {
      DirectHandle<String> value =
          WasmModuleObject::ExtractUtf8StringFromModuleBytes(
              isolate_, wire_bytes, import.field_name, kNoInternalize);
      sanitized_imports_.push_back(value);
      continue;
    }

    if (import.kind == kExternalFunction) {
      WellKnownImport wki = well_known_imports.get(import.index);
      if (IsCompileTimeImport(wki)) {
        DirectHandle<JSFunction> fun =
            CreateFunctionForCompileTimeImport(isolate_, wki);
        sanitized_imports_.push_back(fun);
        continue;
      }
    }

    if (ffi_.is_null()) {
      // No point in continuing if we don't have an imports object.
      thrower_->TypeError(
          "Imports argument must be present and must be an object");
      return;
    }

    DirectHandle<String> module_name =
        WasmModuleObject::ExtractUtf8StringFromModuleBytes(
            isolate_, wire_bytes, import.module_name, kInternalize);

    DirectHandle<String> import_name =
        WasmModuleObject::ExtractUtf8StringFromModuleBytes(
            isolate_, wire_bytes, import.field_name, kInternalize);

    MaybeDirectHandle<Object> result =
        is_asmjs_module(module_)
            ? LookupImportAsm(index, import_name)
            : LookupImport(index, module_name, import_name);
    if (thrower_->error()) {
      return;
    }
    DirectHandle<Object> value = result.ToHandleChecked();
    sanitized_imports_.push_back(value);
  }
}

bool InstanceBuilder::ProcessImportedFunction(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    int import_index, int func_index, DirectHandle<Object> value,
    WellKnownImport preknown_import) {
  // Function imports must be callable.
  if (!IsCallable(*value)) {
    if (!IsWasmSuspendingObject(*value)) {
      thrower_->LinkError("%s: function import requires a callable",
                          ImportName(import_index).c_str());
      return false;
    }
    DCHECK(IsCallable(Cast<WasmSuspendingObject>(*value)->callable()));
  }
  // Store any {WasmExternalFunction} callable in the instance before the call
  // is resolved to preserve its identity. This handles exported functions as
  // well as functions constructed via other means (e.g. WebAssembly.Function).
  if (WasmExternalFunction::IsWasmExternalFunction(*value)) {
    trusted_instance_data->func_refs()->set(
        func_index, Cast<WasmExternalFunction>(*value)->func_ref());
  }
  auto js_receiver = Cast<JSReceiver>(value);
  CanonicalTypeIndex sig_index =
      module_->canonical_sig_id(module_->functions[func_index].sig_index);
  const CanonicalSig* expected_sig =
      GetTypeCanonicalizer()->LookupFunctionSignature(sig_index);
  ResolvedWasmImport resolved(trusted_instance_data, func_index, js_receiver,
                              expected_sig, sig_index, preknown_import);
  if (resolved.well_known_status() != WellKnownImport::kGeneric &&
      v8_flags.trace_wasm_inlining) {
    PrintF("[import %d is well-known built-in %s]\n", import_index,
           WellKnownImportName(resolved.well_known_status()));
  }
  well_known_imports_.push_back(resolved.well_known_status());
  ImportCallKind kind = resolved.kind();
  js_receiver = resolved.callable();
  DirectHandle<WasmFunctionData> trusted_function_data =
      resolved.trusted_function_data();
  ImportedFunctionEntry imported_entry(trusted_instance_data, func_index);
  switch (kind) {
    case ImportCallKind::kRuntimeTypeError:
      imported_entry.SetGenericWasmToJs(
          isolate_, js_receiver, resolved.suspend(), expected_sig, sig_index);
      break;
    case ImportCallKind::kLinkError:
      thrower_->LinkError(
          "%s: imported function does not match the expected type",
          ImportName(import_index).c_str());
      return false;
    case ImportCallKind::kWasmToWasm: {
      // The imported function is a Wasm function from another instance.
      auto function_data =
          Cast<WasmExportedFunctionData>(trusted_function_data);
      // The import reference is the trusted instance data itself.
      Tagged<WasmTrustedInstanceData> instance_data =
          function_data->instance_data();
      CHECK_GE(function_data->function_index(),
               instance_data->module()->num_imported_functions);
      WasmCodePointer imported_target =
          instance_data->GetCallTarget(function_data->function_index());
      imported_entry.SetWasmToWasm(instance_data, imported_target, sig_index
#if V8_ENABLE_DRUMBRAKE
                                   ,
                                   function_data->function_index()
#endif  // V8_ENABLE_DRUMBRAKE
      );
      break;
    }
    case ImportCallKind::kWasmToCapi: {
      int expected_arity = static_cast<int>(expected_sig->parameter_count());
      WasmImportWrapperCache* cache = GetWasmImportWrapperCache();
      WasmCodeRefScope code_ref_scope;
      WasmCode* wasm_code =
          cache->MaybeGet(kind, sig_index, expected_arity, kNoSuspend);
      if (wasm_code == nullptr) {
        {
          WasmImportWrapperCache::ModificationScope cache_scope(cache);
          WasmCompilationResult result =
              compiler::CompileWasmCapiCallWrapper(expected_sig);
          WasmImportWrapperCache::CacheKey key(kind, sig_index, expected_arity,
                                               kNoSuspend);
          wasm_code = cache_scope.AddWrapper(key, std::move(result),
                                             WasmCode::Kind::kWasmToCapiWrapper,
                                             expected_sig->signature_hash());
        }
        // To avoid lock order inversion, code printing must happen after the
        // end of the {cache_scope}.
        wasm_code->MaybePrint();
        isolate_->counters()->wasm_generated_code_size()->Increment(
            wasm_code->instructions().length());
        isolate_->counters()->wasm_reloc_size()->Increment(
            wasm_code->reloc_info().length());
      }

      // We reuse the SetCompiledWasmToJs infrastructure because it passes the
      // callable to the wrapper, which we need to get the function data.
      imported_entry.SetCompiledWasmToJs(isolate_, js_receiver, wasm_code,
                                         kNoSuspend, expected_sig, sig_index);
      break;
    }
    case ImportCallKind::kWasmToJSFastApi: {
      DCHECK(IsJSFunction(*js_receiver) || IsJSBoundFunction(*js_receiver));
      WasmCodeRefScope code_ref_scope;
      // Note: the wrapper we're about to compile is specific to this
      // instantiation, so it cannot be shared. However, its lifetime must
      // be managed by the WasmImportWrapperCache, so that it can be used
      // in WasmDispatchTables whose lifetime might exceed that of this
      // instance's NativeModule.
      // So the {CacheKey} is a dummy, and we don't look for an existing
      // wrapper. Key collisions are not a concern because lifetimes are
      // determined by refcounting.
      WasmCompilationResult result =
          compiler::CompileWasmJSFastCallWrapper(expected_sig, js_receiver);
      WasmCode* wasm_code;
      {
        WasmImportWrapperCache::ModificationScope cache_scope(
            GetWasmImportWrapperCache());
        WasmImportWrapperCache::CacheKey dummy_key(kind, CanonicalTypeIndex{0},
                                                   0, kNoSuspend);
        wasm_code = cache_scope.AddWrapper(dummy_key, std::move(result),
                                           WasmCode::Kind::kWasmToJsWrapper,
                                           expected_sig->signature_hash());
      }
      // To avoid lock order inversion, code printing must happen after the
      // end of the {cache_scope}.
      wasm_code->MaybePrint();
      imported_entry.SetCompiledWasmToJs(isolate_, js_receiver, wasm_code,
                                         kNoSuspend, expected_sig, sig_index);
      break;
    }
    default: {
      // The imported function is a callable.
      if (UseGenericWasmToJSWrapper(kind, expected_sig, resolved.suspend())) {
        DCHECK(kind == ImportCallKind::kJSFunctionArityMatch ||
               kind == ImportCallKind::kJSFunctionArityMismatch);
        imported_entry.SetGenericWasmToJs(
            isolate_, js_receiver, resolved.suspend(), expected_sig, sig_index);
        break;
      }
      if (v8_flags.wasm_jitless) {
        WasmCode* no_code = nullptr;
        imported_entry.SetCompiledWasmToJs(isolate_, js_receiver, no_code,
                                           resolved.suspend(), expected_sig,
                                           sig_index);
        break;
      }
      int expected_arity = static_cast<int>(expected_sig->parameter_count());
      if (kind == ImportCallKind::kJSFunctionArityMismatch) {
        auto function = Cast<JSFunction>(js_receiver);
        Tagged<SharedFunctionInfo> shared = function->shared();
        expected_arity =
            shared->internal_formal_parameter_count_without_receiver();
      }

      WasmImportWrapperCache* cache = GetWasmImportWrapperCache();
      WasmCodeRefScope code_ref_scope;
      WasmCode* wasm_code =
          cache->MaybeGet(kind, sig_index, expected_arity, resolved.suspend());
      if (!wasm_code) {
        // This should be a very rare fallback case. We expect that the
        // generic wrapper will be used (see above).
        bool source_positions =
            is_asmjs_module(trusted_instance_data->module());
        wasm_code = cache->CompileWasmImportCallWrapper(
            isolate_, kind, expected_sig, sig_index, source_positions,
            expected_arity, resolved.suspend());
      }

      DCHECK_NOT_NULL(wasm_code);
      CHECK_EQ(wasm_code->kind(), WasmCode::kWasmToJsWrapper);
      // Wasm to JS wrappers are treated specially in the import table.
      imported_entry.SetCompiledWasmToJs(isolate_, js_receiver, wasm_code,
                                         resolved.suspend(), expected_sig,
                                         sig_index);
      break;
    }
  }
  return true;
}

bool InstanceBuilder::ProcessImportedTable(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    int import_index, int table_index, DirectHandle<Object> value) {
  if (!IsWasmTableObject(*value)) {
    thrower_->LinkError("%s: table import requires a WebAssembly.Table",
                        ImportName(import_index).c_str());
    return false;
  }
  const WasmTable& table = module_->tables[table_index];

  DirectHandle<WasmTableObject> table_object = Cast<WasmTableObject>(value);

  uint32_t imported_table_size =
      static_cast<uint32_t>(table_object->current_length());
  if (imported_table_size < table.initial_size) {
    thrower_->LinkError("table import %d is smaller than initial %u, got %u",
                        import_index, table.initial_size, imported_table_size);
    return false;
  }

  if (table.has_maximum_size) {
    std::optional<uint64_t> max_size = table_object->maximum_length_u64();
    if (!max_size) {
      thrower_->LinkError(
          "table import %d has no maximum length; required: %" PRIu64,
          import_index, table.maximum_size);
      return false;
    }
    if (*max_size > table.maximum_size) {
      thrower_->LinkError("table import %d has a larger maximum size %" PRIx64
                          " than the module's declared maximum %" PRIu64,
                          import_index, *max_size, table.maximum_size);
      return false;
    }
  }

  if (table.address_type != table_object->address_type()) {
    thrower_->LinkError("cannot import %s table as %s",
                        AddressTypeToStr(table_object->address_type()),
                        AddressTypeToStr(table.address_type));
    return false;
  }

  const WasmModule* table_type_module =
      table_object->has_trusted_data()
          ? table_object->trusted_data(isolate_)->module()
          : nullptr;
  // The security-relevant aspect of this DCHECK is covered by the SBXCHECK_EQ
  // below.
  DCHECK_IMPLIES(table_object->unsafe_type().has_index(),
                 table_type_module != nullptr);

  if (!EquivalentTypes(table.type, table_object->type(table_type_module),
                       module_, table_type_module)) {
    thrower_->LinkError("%s: imported table does not match the expected type",
                        ImportName(import_index).c_str());
    return false;
  }

  // Note: {trusted_instance_data} is selected by the caller to be the
  // shared or non-shared part, depending on {table.shared}.
  trusted_instance_data->tables()->set(table_index, *table_object);
  if (table_object->has_trusted_dispatch_table()) {
    Tagged<WasmDispatchTable> dispatch_table =
        table_object->trusted_dispatch_table(isolate_);
    SBXCHECK_EQ(dispatch_table->table_type(),
                module_->canonical_type(table.type));
    SBXCHECK_GE(dispatch_table->length(), table.initial_size);
    trusted_instance_data->dispatch_tables()->set(table_index, dispatch_table);
    if (table_index == 0) {
      trusted_instance_data->set_dispatch_table0(dispatch_table);
    }
  } else {
    // Function tables are required to have a WasmDispatchTable.
    SBXCHECK(!IsSubtypeOf(table.type, kWasmFuncRef, module_));
  }
  return true;
}

bool InstanceBuilder::ProcessImportedWasmGlobalObject(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    int import_index, const WasmGlobal& global,
    DirectHandle<WasmGlobalObject> global_object) {
  if (static_cast<bool>(global_object->is_mutable()) != global.mutability) {
    thrower_->LinkError(
        "%s: imported global does not match the expected mutability",
        ImportName(import_index).c_str());
    return false;
  }

  wasm::ValueType actual_type = global_object->type();
  const WasmModule* source_module = nullptr;
  if (global_object->has_trusted_data()) {
    source_module = global_object->trusted_data(isolate_)->module();
    SBXCHECK(!actual_type.has_index() ||
             source_module->has_type(actual_type.ref_index()));
  } else {
    // We don't have a module, so we wouldn't know what to do with a
    // module-relative type index.
    // Note: since we just read a type from the untrusted heap, this can't
    // be a real security boundary; we just use SBXCHECK to make it obvious
    // to fuzzers that crashing here due to corruption is safe.
    SBXCHECK(!actual_type.has_index());
  }

  bool valid_type =
      global.mutability
          ? EquivalentTypes(actual_type, global.type, source_module, module_)
          : IsSubtypeOf(actual_type, global.type, source_module, module_);

  if (!valid_type) {
    thrower_->LinkError("%s: imported global does not match the expected type",
                        ImportName(import_index).c_str());
    return false;
  }
  if (global.mutability) {
    DCHECK_LT(global.index, module_->num_imported_mutable_globals);
    DirectHandle<Object> buffer;
    if (global.type.is_reference()) {
      static_assert(sizeof(global_object->offset()) <= sizeof(Address),
                    "The offset into the globals buffer does not fit into "
                    "the imported_mutable_globals array");
      buffer = direct_handle(global_object->tagged_buffer(), isolate_);
      // For externref globals we use a relative offset, not an absolute
      // address.
      trusted_instance_data->imported_mutable_globals()->set(
          global.index, global_object->offset());
    } else {
      buffer = direct_handle(global_object->untagged_buffer(), isolate_);
      // It is safe in this case to store the raw pointer to the buffer
      // since the backing store of the JSArrayBuffer will not be
      // relocated.
      Address address = reinterpret_cast<Address>(
          raw_buffer_ptr(Cast<JSArrayBuffer>(buffer), global_object->offset()));
      trusted_instance_data->imported_mutable_globals()->set_sandboxed_pointer(
          global.index, address);
    }
    trusted_instance_data->imported_mutable_globals_buffers()->set(global.index,
                                                                   *buffer);
    return true;
  }

  WasmValue value;
  switch (global.type.kind()) {
    case kI32:
      value = WasmValue(global_object->GetI32());
      break;
    case kI64:
      value = WasmValue(global_object->GetI64());
      break;
    case kF32:
      value = WasmValue(global_object->GetF32());
      break;
    case kF64:
      value = WasmValue(global_object->GetF64());
      break;
    case kS128:
      value = WasmValue(global_object->GetS128RawBytes(), kCanonicalS128);
      break;
    case kRef:
    case kRefNull:
      value = WasmValue(global_object->GetRef(),
                        module_->canonical_type(global.type));
      break;
    case kVoid:
    case kTop:
    case kBottom:
    case kI8:
    case kI16:
    case kF16:
      UNREACHABLE();
  }

  WriteGlobalValue(global, value);
  return true;
}

bool InstanceBuilder::ProcessImportedGlobal(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    int import_index, int global_index, DirectHandle<Object> value) {
  // Immutable global imports are converted to numbers and written into
  // the {untagged_globals_} array buffer.
  //
  // Mutable global imports instead have their backing array buffers
  // referenced by this instance, and store the address of the imported
  // global in the {imported_mutable_globals_} array.
  const WasmGlobal& global = module_->globals[global_index];

  // SIMD proposal allows modules to define an imported v128 global, and only
  // supports importing a WebAssembly.Global object for this global, but also
  // defines constructing a WebAssembly.Global of v128 to be a TypeError.
  // We *should* never hit this case in the JS API, but the module should should
  // be allowed to declare such a global (no validation error).
  if (global.type == kWasmS128 && !IsWasmGlobalObject(*value)) {
    thrower_->LinkError(
        "%s: global import of type v128 must be a WebAssembly.Global",
        ImportName(import_index).c_str());
    return false;
  }

  if (is_asmjs_module(module_)) {
    // Accepting {JSFunction} on top of just primitive values here is a
    // workaround to support legacy asm.js code with broken binding. Note
    // that using {NaN} (or Smi::zero()) here is what using the observable
    // conversion via {ToPrimitive} would produce as well. {LookupImportAsm}
    // checked via {HasDefaultToNumberBehaviour} that "valueOf" or friends have
    // not been patched.
    if (IsJSFunction(*value)) value = isolate_->factory()->nan_value();
    if (IsPrimitive(*value)) {
      MaybeDirectHandle<Object> converted =
          global.type == kWasmI32 ? Object::ToInt32(isolate_, value)
                                  : Object::ToNumber(isolate_, value);
      if (!converted.ToHandle(&value)) {
        // Conversion is known to fail for Symbols and BigInts.
        thrower_->LinkError("%s: global import must be a number",
                            ImportName(import_index).c_str());
        return false;
      }
    }
  }

  if (IsWasmGlobalObject(*value)) {
    auto global_object = Cast<WasmGlobalObject>(value);
    return ProcessImportedWasmGlobalObject(trusted_instance_data, import_index,
                                           global, global_object);
  }

  if (global.mutability) {
    thrower_->LinkError(
        "%s: imported mutable global must be a WebAssembly.Global object",
        ImportName(import_index).c_str());
    return false;
  }

  if (global.type.is_reference()) {
    const char* error_message;
    DirectHandle<Object> wasm_value;
    if (!wasm::JSToWasmObject(isolate_, module_, value, global.type,
                              &error_message)
             .ToHandle(&wasm_value)) {
      thrower_->LinkError("%s: %s", ImportName(import_index).c_str(),
                          error_message);
      return false;
    }
    WriteGlobalValue(
        global, WasmValue(wasm_value, module_->canonical_type(global.type)));
    return true;
  }

  if (IsNumber(*value) && global.type != kWasmI64) {
    double number_value = Object::NumberValue(*value);
    // The Wasm-BigInt proposal currently says that i64 globals may
    // only be initialized with BigInts. See:
    // https://github.com/WebAssembly/JS-BigInt-integration/issues/12
    WasmValue wasm_value =
        global.type == kWasmI32   ? WasmValue(DoubleToInt32(number_value))
        : global.type == kWasmF32 ? WasmValue(DoubleToFloat32(number_value))
                                  : WasmValue(number_value);
    WriteGlobalValue(global, wasm_value);
    return true;
  }

  if (global.type == kWasmI64 && IsBigInt(*value)) {
    WriteGlobalValue(global, WasmValue(Cast<BigInt>(*value)->AsInt64()));
    return true;
  }

  thrower_->LinkError(
      "%s: global import must be a number, valid Wasm reference, or "
      "WebAssembly.Global object",
      ImportName(import_index).c_str());
  return false;
}

// Process the imports, including functions, tables, globals, and memory, in
// order, loading them from the {ffi_} object. Returns the number of imported
// functions.
int InstanceBuilder::ProcessImports(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  int num_imported_functions = 0;
  int num_imported_tables = 0;

  DCHECK_EQ(module_->import_table.size(), sanitized_imports_.size());

  const WellKnownImportsList& preknown_imports =
      module_->type_feedback.well_known_imports;
  int num_imports = static_cast<int>(module_->import_table.size());
  for (int index = 0; index < num_imports; ++index) {
    const WasmImport& import = module_->import_table[index];

    DirectHandle<Object> value = sanitized_imports_[index];

    switch (import.kind) {
      case kExternalFunction: {
        uint32_t func_index = import.index;
        DCHECK_EQ(num_imported_functions, func_index);
        ModuleTypeIndex sig_index = module_->functions[func_index].sig_index;
        bool function_is_shared = module_->type(sig_index).is_shared;
        if (!ProcessImportedFunction(
                function_is_shared ? shared_trusted_instance_data
                                   : trusted_instance_data,
                index, func_index, value, preknown_imports.get(func_index))) {
          return -1;
        }
        num_imported_functions++;
        break;
      }
      case kExternalTable: {
        uint32_t table_index = import.index;
        DCHECK_EQ(table_index, num_imported_tables);
        bool table_is_shared = module_->tables[table_index].shared;
        if (!ProcessImportedTable(table_is_shared ? shared_trusted_instance_data
                                                  : trusted_instance_data,
                                  index, table_index, value)) {
          return -1;
        }
        num_imported_tables++;
        USE(num_imported_tables);
        break;
      }
      case kExternalMemory:
        // Imported memories are already handled earlier via
        // {ProcessImportedMemories}.
        break;
      case kExternalGlobal: {
        bool global_is_shared = module_->globals[import.index].shared;
        if (!ProcessImportedGlobal(global_is_shared
                                       ? shared_trusted_instance_data
                                       : trusted_instance_data,
                                   index, import.index, value)) {
          return -1;
        }
        break;
      }
      case kExternalTag: {
        // TODO(14616): Implement shared tags.
        if (!IsWasmTagObject(*value)) {
          thrower_->LinkError("%s: tag import requires a WebAssembly.Tag",
                              ImportName(index).c_str());
          return -1;
        }
        DirectHandle<WasmTagObject> imported_tag = Cast<WasmTagObject>(value);
        if (!imported_tag->MatchesSignature(module_->canonical_sig_id(
                module_->tags[import.index].sig_index))) {
          thrower_->LinkError(
              "%s: imported tag does not match the expected type",
              ImportName(index).c_str());
          return -1;
        }
        Tagged<Object> tag = imported_tag->tag();
        DCHECK(IsUndefined(
            trusted_instance_data->tags_table()->get(import.index)));
        trusted_instance_data->tags_table()->set(import.index, tag);
        tags_wrappers_[import.index] = imported_tag;
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  if (num_imported_functions > 0) {
    module_object_->native_module()->UpdateWellKnownImports(
        base::VectorOf(well_known_imports_));
  }
  return num_imported_functions;
}

bool InstanceBuilder::ProcessImportedMemories(
    DirectHandle<FixedArray> imported_memory_objects) {
  DCHECK_EQ(module_->import_table.size(), sanitized_imports_.size());

  int num_imports = static_cast<int>(module_->import_table.size());
  for (int import_index = 0; import_index < num_imports; ++import_index) {
    const WasmImport& import = module_->import_table[import_index];

    if (import.kind != kExternalMemory) continue;

    DirectHandle<Object> value = sanitized_imports_[import_index];

    if (!IsWasmMemoryObject(*value)) {
      thrower_->LinkError(
          "%s: memory import must be a WebAssembly.Memory object",
          ImportName(import_index).c_str());
      return false;
    }
    uint32_t memory_index = import.index;
    auto memory_object = Cast<WasmMemoryObject>(value);

    DirectHandle<JSArrayBuffer> buffer{memory_object->array_buffer(), isolate_};
    uint32_t imported_cur_pages =
        static_cast<uint32_t>(buffer->byte_length() / kWasmPageSize);
    const WasmMemory* memory = &module_->memories[memory_index];
    if (memory->address_type != memory_object->address_type()) {
      thrower_->LinkError("cannot import %s memory as %s",
                          AddressTypeToStr(memory_object->address_type()),
                          AddressTypeToStr(memory->address_type));
      return false;
    }
    if (imported_cur_pages < memory->initial_pages) {
      thrower_->LinkError(
          "%s: memory import has %u pages which is smaller than the declared "
          "initial of %u",
          ImportName(import_index).c_str(), imported_cur_pages,
          memory->initial_pages);
      return false;
    }
    int32_t imported_maximum_pages = memory_object->maximum_pages();
    if (memory->has_maximum_pages) {
      if (imported_maximum_pages < 0) {
        thrower_->LinkError(
            "%s: memory import has no maximum limit, expected at most %u",
            ImportName(import_index).c_str(), imported_maximum_pages);
        return false;
      }
      if (static_cast<uint64_t>(imported_maximum_pages) >
          memory->maximum_pages) {
        thrower_->LinkError(
            "%s: memory import has a larger maximum size %u than the "
            "module's declared maximum %" PRIu64,
            ImportName(import_index).c_str(), imported_maximum_pages,
            memory->maximum_pages);
        return false;
      }
    }
    if (memory->is_shared != buffer->is_shared()) {
      thrower_->LinkError(
          "%s: mismatch in shared state of memory, declared = %d, imported = "
          "%d",
          ImportName(import_index).c_str(), memory->is_shared,
          buffer->is_shared());
      return false;
    }

    DCHECK_EQ(ReadOnlyRoots{isolate_}.undefined_value(),
              imported_memory_objects->get(memory_index));
    imported_memory_objects->set(memory_index, *memory_object);
  }
  return true;
}

template <typename T>
T* InstanceBuilder::GetRawUntaggedGlobalPtr(const WasmGlobal& global) {
  return reinterpret_cast<T*>(raw_buffer_ptr(
      global.shared ? shared_untagged_globals_ : untagged_globals_,
      global.offset));
}

// Process initialization of globals.
void InstanceBuilder::InitGlobals(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  for (const WasmGlobal& global : module_->globals) {
    DCHECK_IMPLIES(global.imported, !global.init.is_set());
    if (!global.init.is_set()) continue;

    ValueOrError result = EvaluateConstantExpression(
        &init_expr_zone_, global.init, global.type, module_, isolate_,
        trusted_instance_data, shared_trusted_instance_data);
    if (MaybeMarkError(result, thrower_)) return;

    if (global.type.is_reference()) {
      (global.shared ? shared_tagged_globals_ : tagged_globals_)
          ->set(global.offset, *to_value(result).to_ref());
    } else {
      to_value(result).CopyTo(GetRawUntaggedGlobalPtr<uint8_t>(global));
    }
  }
}

// Allocate memory for a module instance as a new JSArrayBuffer.
MaybeDirectHandle<WasmMemoryObject> InstanceBuilder::AllocateMemory(
    uint32_t memory_index) {
  const WasmMemory& memory = module_->memories[memory_index];
  int initial_pages = static_cast<int>(memory.initial_pages);
  int maximum_pages = memory.has_maximum_pages
                          ? static_cast<int>(memory.maximum_pages)
                          : WasmMemoryObject::kNoMaximum;
  auto shared = memory.is_shared ? SharedFlag::kShared : SharedFlag::kNotShared;

  MaybeDirectHandle<WasmMemoryObject> maybe_memory_object =
      WasmMemoryObject::New(isolate_, initial_pages, maximum_pages, shared,
                            memory.address_type);
  if (maybe_memory_object.is_null()) {
    thrower_->RangeError(
        "Out of memory: Cannot allocate Wasm memory for new instance");
    return {};
  }
  return maybe_memory_object;
}

// Process the exports, creating wrappers for functions, tables, memories,
// globals, and exceptions.
void InstanceBuilder::ProcessExports(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  std::unordered_map<int, IndirectHandle<Object>> imported_globals;

  // If an imported WebAssembly global gets exported, the export has to be
  // identical to to import. Therefore we cache all re-exported globals
  // in a map here.
  // Note: re-exported functions must also preserve their identity; they
  // have already been cached in the instance by {ProcessImportedFunction}.
  for (size_t index = 0, end = module_->import_table.size(); index < end;
       ++index) {
    const WasmImport& import = module_->import_table[index];
    if (import.kind == kExternalGlobal &&
        module_->globals[import.index].exported) {
      DirectHandle<Object> value = sanitized_imports_[index];
      if (IsWasmGlobalObject(*value)) {
        imported_globals[import.index] = indirect_handle(value, isolate_);
      }
    }
  }

  DirectHandle<WasmInstanceObject> instance_object{
      trusted_instance_data->instance_object(), isolate_};
  DirectHandle<JSObject> exports_object =
      direct_handle(instance_object->exports_object(), isolate_);
  MaybeDirectHandle<String> single_function_name;
  bool is_asm_js = is_asmjs_module(module_);
  if (is_asm_js) {
    DirectHandle<JSFunction> object_function = DirectHandle<JSFunction>(
        isolate_->native_context()->object_function(), isolate_);
    exports_object = isolate_->factory()->NewJSObject(object_function);
    single_function_name =
        isolate_->factory()->InternalizeUtf8String(AsmJs::kSingleFunctionName);
    instance_object->set_exports_object(*exports_object);
  }

  // Switch the exports object to dictionary mode and allocate enough storage
  // for the expected number of exports.
  DCHECK(exports_object->HasFastProperties());
  JSObject::NormalizeProperties(
      isolate_, exports_object, KEEP_INOBJECT_PROPERTIES,
      static_cast<int>(module_->export_table.size()), "WasmExportsObject");

  PropertyDescriptor desc;
  desc.set_writable(is_asm_js);
  desc.set_enumerable(true);
  desc.set_configurable(is_asm_js);

  const PropertyDetails details{PropertyKind::kData, desc.ToAttributes(),
                                PropertyConstness::kMutable};

  // Process each export in the export table.
  for (const WasmExport& exp : module_->export_table) {
    DirectHandle<String> name =
        WasmModuleObject::ExtractUtf8StringFromModuleBytes(
            isolate_, module_object_, exp.name, kInternalize);
    DirectHandle<JSAny> value;
    switch (exp.kind) {
      case kExternalFunction: {
        // Wrap and export the code as a JSFunction.
        bool shared = module_->function_is_shared(exp.index);
        DirectHandle<WasmFuncRef> func_ref =
            WasmTrustedInstanceData::GetOrCreateFuncRef(
                isolate_,
                shared ? shared_trusted_instance_data : trusted_instance_data,
                exp.index);
        DirectHandle<WasmInternalFunction> internal_function{
            func_ref->internal(isolate_), isolate_};
        DirectHandle<JSFunction> wasm_external_function =
            WasmInternalFunction::GetOrCreateExternal(internal_function);
        value = wasm_external_function;

        if (is_asm_js &&
            String::Equals(isolate_, name,
                           single_function_name.ToHandleChecked())) {
          desc.set_value(value);
          CHECK(JSReceiver::DefineOwnProperty(isolate_, instance_object, name,
                                              &desc, Just(kThrowOnError))
                    .FromMaybe(false));
          continue;
        }
        break;
      }
      case kExternalTable: {
        bool shared = module_->tables[exp.index].shared;
        DirectHandle<WasmTrustedInstanceData> data =
            shared ? shared_trusted_instance_data : trusted_instance_data;
        value = direct_handle(Cast<JSAny>(data->tables()->get(exp.index)),
                              isolate_);
        break;
      }
      case kExternalMemory: {
        // Export the memory as a WebAssembly.Memory object. A WasmMemoryObject
        // should already be available if the module has memory, since we always
        // create or import it when building an WasmInstanceObject.
        value = direct_handle(trusted_instance_data->memory_object(exp.index),
                              isolate_);
        break;
      }
      case kExternalGlobal: {
        const WasmGlobal& global = module_->globals[exp.index];
        DirectHandle<WasmTrustedInstanceData>
            maybe_shared_trusted_instance_data =
                global.shared ? shared_trusted_instance_data
                              : trusted_instance_data;
        if (global.imported) {
          auto cached_global = imported_globals.find(exp.index);
          if (cached_global != imported_globals.end()) {
            value = Cast<JSAny>(cached_global->second);
            break;
          }
        }
        DirectHandle<JSArrayBuffer> untagged_buffer;
        DirectHandle<FixedArray> tagged_buffer;
        uint32_t offset;

        if (global.mutability && global.imported) {
          DirectHandle<FixedArray> buffers_array(
              maybe_shared_trusted_instance_data
                  ->imported_mutable_globals_buffers(),
              isolate_);
          if (global.type.is_reference()) {
            tagged_buffer = direct_handle(
                Cast<FixedArray>(buffers_array->get(global.index)), isolate_);
            // For externref globals we store the relative offset in the
            // imported_mutable_globals array instead of an absolute address.
            offset = static_cast<uint32_t>(
                maybe_shared_trusted_instance_data->imported_mutable_globals()
                    ->get(global.index));
          } else {
            untagged_buffer = direct_handle(
                Cast<JSArrayBuffer>(buffers_array->get(global.index)),
                isolate_);
            Address global_addr =
                maybe_shared_trusted_instance_data->imported_mutable_globals()
                    ->get_sandboxed_pointer(global.index);

            size_t buffer_size = untagged_buffer->byte_length();
            Address backing_store =
                reinterpret_cast<Address>(untagged_buffer->backing_store());
            CHECK(global_addr >= backing_store &&
                  global_addr < backing_store + buffer_size);
            offset = static_cast<uint32_t>(global_addr - backing_store);
          }
        } else {
          if (global.type.is_reference()) {
            tagged_buffer = direct_handle(
                maybe_shared_trusted_instance_data->tagged_globals_buffer(),
                isolate_);
          } else {
            untagged_buffer = direct_handle(
                maybe_shared_trusted_instance_data->untagged_globals_buffer(),
                isolate_);
          }
          offset = global.offset;
        }

        // Since the global's array untagged_buffer is always provided,
        // allocation should never fail.
        DirectHandle<WasmGlobalObject> global_obj =
            WasmGlobalObject::New(isolate_,
                                  global.shared ? shared_trusted_instance_data
                                                : trusted_instance_data,
                                  untagged_buffer, tagged_buffer, global.type,
                                  offset, global.mutability)
                .ToHandleChecked();
        value = global_obj;
        break;
      }
      case kExternalTag: {
        const WasmTag& tag = module_->tags[exp.index];
        DirectHandle<WasmTagObject> wrapper = tags_wrappers_[exp.index];
        if (wrapper.is_null()) {
          DirectHandle<HeapObject> tag_object(
              Cast<HeapObject>(
                  trusted_instance_data->tags_table()->get(exp.index)),
              isolate_);
          CanonicalTypeIndex sig_index =
              module_->canonical_sig_id(tag.sig_index);
          // TODO(42204563): Support shared tags.
          wrapper = WasmTagObject::New(isolate_, tag.sig, sig_index, tag_object,
                                       trusted_instance_data);
          tags_wrappers_[exp.index] = wrapper;
        }
        value = wrapper;
        break;
      }
      default:
        UNREACHABLE();
    }

    uint32_t index;
    if (V8_UNLIKELY(name->AsArrayIndex(&index))) {
      // Add a data element.
      JSObject::AddDataElement(exports_object, index, value,
                               details.attributes());
    } else {
      // Add a property to the dictionary.
      JSObject::SetNormalizedProperty(exports_object, name, value, details);
    }
  }

  // Switch back to fast properties if possible.
  JSObject::MigrateSlowToFast(exports_object, 0, "WasmExportsObjectFinished");

  if (module_->origin == kWasmOrigin) {
    CHECK(JSReceiver::SetIntegrityLevel(isolate_, exports_object, FROZEN,
                                        kDontThrow)
              .FromMaybe(false));
  }
}

namespace {
V8_INLINE void SetFunctionTablePlaceholder(
    Isolate* isolate,
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTableObject> table_object, uint32_t entry_index,
    uint32_t func_index) {
  const WasmModule* module = trusted_instance_data->module();
  const WasmFunction* function = &module->functions[func_index];
  Tagged<WasmFuncRef> func_ref;
  if (trusted_instance_data->try_get_func_ref(func_index, &func_ref)) {
    table_object->entries()->set(entry_index, *func_ref);
  } else {
    WasmTableObject::SetFunctionTablePlaceholder(
        isolate, table_object, entry_index, trusted_instance_data, func_index);
  }
  WasmTableObject::UpdateDispatchTable(isolate, table_object, entry_index,
                                       function, trusted_instance_data
#if V8_ENABLE_DRUMBRAKE
                                       ,
                                       func_index
#endif  // V8_ENABLE_DRUMBRAKE
  );
}

V8_INLINE void SetFunctionTableNullEntry(
    Isolate* isolate, DirectHandle<WasmTableObject> table_object,
    uint32_t entry_index) {
  table_object->entries()->set(entry_index, ReadOnlyRoots{isolate}.wasm_null());
  table_object->ClearDispatchTable(entry_index);
}
}  // namespace

void InstanceBuilder::SetTableInitialValues(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  for (int table_index = 0;
       table_index < static_cast<int>(module_->tables.size()); ++table_index) {
    const WasmTable& table = module_->tables[table_index];
    DirectHandle<WasmTrustedInstanceData> maybe_shared_trusted_instance_data =
        table.shared ? shared_trusted_instance_data : trusted_instance_data;
    // We must not modify imported tables yet when this is run, because
    // we can't know yet whether the new instance can be successfully
    // initialized.
    DCHECK_IMPLIES(table.imported, !table.initial_value.is_set());
    if (!table.initial_value.is_set()) continue;
    DirectHandle<WasmTableObject> table_object(
        Cast<WasmTableObject>(
            maybe_shared_trusted_instance_data->tables()->get(table_index)),
        isolate_);
    bool is_function_table = IsSubtypeOf(table.type, kWasmFuncRef, module_);
    if (is_function_table &&
        table.initial_value.kind() == ConstantExpression::Kind::kRefFunc) {
      for (uint32_t entry_index = 0; entry_index < table.initial_size;
           entry_index++) {
        SetFunctionTablePlaceholder(
            isolate_, maybe_shared_trusted_instance_data, table_object,
            entry_index, table.initial_value.index());
      }
    } else if (is_function_table && table.initial_value.kind() ==
                                        ConstantExpression::Kind::kRefNull) {
      for (uint32_t entry_index = 0; entry_index < table.initial_size;
           entry_index++) {
        SetFunctionTableNullEntry(isolate_, table_object, entry_index);
      }
    } else {
      ValueOrError result = EvaluateConstantExpression(
          &init_expr_zone_, table.initial_value, table.type, module_, isolate_,
          maybe_shared_trusted_instance_data, shared_trusted_instance_data);
      if (MaybeMarkError(result, thrower_)) return;
      for (uint32_t entry_index = 0; entry_index < table.initial_size;
           entry_index++) {
        WasmTableObject::Set(isolate_, table_object, entry_index,
                             to_value(result).to_ref());
      }
    }
  }
}

namespace {

enum FunctionComputationMode { kLazyFunctionsAndNull, kStrictFunctionsAndNull };

// If {function_mode == kLazyFunctionsAndNull}, may return a function index
// instead of computing a function object, and {WasmValue(-1)} instead of null.
// Assumes the underlying module is verified.
// Resets {zone}, so make sure it contains no useful data.
ValueOrError ConsumeElementSegmentEntry(
    Zone* zone, Isolate* isolate,
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data,
    const WasmElemSegment& segment, Decoder& decoder,
    FunctionComputationMode function_mode) {
  const WasmModule* module = trusted_instance_data->module();
  if (segment.element_type == WasmElemSegment::kFunctionIndexElements) {
    uint32_t function_index = decoder.consume_u32v();
    return function_mode == kStrictFunctionsAndNull
               ? EvaluateConstantExpression(
                     zone, ConstantExpression::RefFunc(function_index),
                     segment.type, module, isolate, trusted_instance_data,
                     shared_trusted_instance_data)
               : ValueOrError(WasmValue(function_index));
  }

  switch (static_cast<WasmOpcode>(*decoder.pc())) {
    case kExprRefFunc: {
      auto [function_index, length] =
          decoder.read_u32v<Decoder::FullValidationTag>(decoder.pc() + 1,
                                                        "ref.func");
      if (V8_LIKELY(decoder.lookahead(1 + length, kExprEnd))) {
        decoder.consume_bytes(length + 2);
        return function_mode == kStrictFunctionsAndNull
                   ? EvaluateConstantExpression(
                         zone, ConstantExpression::RefFunc(function_index),
                         segment.type, module, isolate, trusted_instance_data,
                         shared_trusted_instance_data)
                   : ValueOrError(WasmValue(function_index));
      }
      break;
    }
    case kExprRefNull: {
      auto [heap_type, length] =
          value_type_reader::read_heap_type<Decoder::FullValidationTag>(
              &decoder, decoder.pc() + 1, WasmEnabledFeatures::All());
      value_type_reader::Populate(&heap_type, module);
      if (V8_LIKELY(decoder.lookahead(1 + length, kExprEnd))) {
        decoder.consume_bytes(length + 2);
        return function_mode == kStrictFunctionsAndNull
                   ? EvaluateConstantExpression(
                         zone, ConstantExpression::RefNull(heap_type),
                         segment.type, module, isolate, trusted_instance_data,
                         shared_trusted_instance_data)
                   : WasmValue(int32_t{-1});
      }
      break;
    }
    default:
      break;
  }

  auto sig = FixedSizeSignature<ValueType>::Returns(segment.type);
  constexpr bool kIsShared = false;  // TODO(14616): Is this correct?
  FunctionBody body(&sig, decoder.pc_offset(), decoder.pc(), decoder.end(),
                    kIsShared);
  WasmDetectedFeatures detected;
  ValueOrError result;
  {
    // We need a scope for the decoder because its destructor resets some Zone
    // elements, which has to be done before we reset the Zone afterwards.
    // We use FullValidationTag so we do not have to create another template
    // instance of WasmFullDecoder, which would cost us >50Kb binary code
    // size.
    WasmFullDecoder<Decoder::FullValidationTag, ConstantExpressionInterface,
                    kConstantExpression>
        full_decoder(zone, trusted_instance_data->module(),
                     WasmEnabledFeatures::All(), &detected, body,
                     trusted_instance_data->module(), isolate,
                     trusted_instance_data, shared_trusted_instance_data);

    full_decoder.DecodeFunctionBody();

    decoder.consume_bytes(static_cast<int>(full_decoder.pc() - decoder.pc()));

    result = full_decoder.interface().has_error()
                 ? ValueOrError(full_decoder.interface().error())
                 : ValueOrError(full_decoder.interface().computed_value());
  }

  zone->Reset();

  return result;
}

}  // namespace

std::optional<MessageTemplate> InitializeElementSegment(
    Zone* zone, Isolate* isolate,
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data,
    uint32_t segment_index) {
  bool shared =
      trusted_instance_data->module()->elem_segments[segment_index].shared;
  DirectHandle<WasmTrustedInstanceData> data =
      shared ? shared_trusted_instance_data : trusted_instance_data;
  if (!IsUndefined(data->element_segments()->get(segment_index))) return {};

  const NativeModule* native_module = data->native_module();
  const WasmModule* module = native_module->module();
  const WasmElemSegment& elem_segment = module->elem_segments[segment_index];

  base::Vector<const uint8_t> module_bytes = native_module->wire_bytes();

  Decoder decoder(module_bytes);
  decoder.consume_bytes(elem_segment.elements_wire_bytes_offset);

  DirectHandle<FixedArray> result =
      isolate->factory()->NewFixedArray(elem_segment.element_count);

  for (size_t i = 0; i < elem_segment.element_count; ++i) {
    ValueOrError value = ConsumeElementSegmentEntry(
        zone, isolate, trusted_instance_data, shared_trusted_instance_data,
        elem_segment, decoder, kStrictFunctionsAndNull);
    if (is_error(value)) return {to_error(value)};
    result->set(static_cast<int>(i), *to_value(value).to_ref());
  }

  data->element_segments()->set(segment_index, *result);

  return {};
}

void InstanceBuilder::LoadTableSegments(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data,
    DirectHandle<WasmTrustedInstanceData> shared_trusted_instance_data) {
  for (uint32_t segment_index = 0;
       segment_index < module_->elem_segments.size(); ++segment_index) {
    const WasmElemSegment& elem_segment = module_->elem_segments[segment_index];
    // Passive segments are not copied during instantiation.
    if (elem_segment.status != WasmElemSegment::kStatusActive) continue;

    const uint32_t table_index = elem_segment.table_index;

    const WasmTable* table = &module_->tables[table_index];
    size_t dest_offset;
    ValueOrError result = EvaluateConstantExpression(
        &init_expr_zone_, elem_segment.offset,
        table->is_table64() ? kWasmI64 : kWasmI32, module_, isolate_,
        trusted_instance_data, shared_trusted_instance_data);
    if (MaybeMarkError(result, thrower_)) return;
    if (table->is_table64()) {
      uint64_t dest_offset_64 = to_value(result).to_u64();
      // Clamp to {std::numeric_limits<size_t>::max()}, which is always an
      // invalid offset, so we always fail the bounds check below.
      DCHECK_GT(std::numeric_limits<size_t>::max(), wasm::max_table_size());
      dest_offset = static_cast<size_t>(std::min(
          dest_offset_64, uint64_t{std::numeric_limits<size_t>::max()}));
    } else {
      dest_offset = to_value(result).to_u32();
    }

    const size_t count = elem_segment.element_count;

    DirectHandle<WasmTableObject> table_object(
        Cast<WasmTableObject>((table->shared ? shared_trusted_instance_data
                                             : trusted_instance_data)
                                  ->tables()
                                  ->get(table_index)),
        isolate_);
    if (!base::IsInBounds<size_t>(dest_offset, count,
                                  table_object->current_length())) {
      thrower_->RuntimeError("%s",
                             MessageFormatter::TemplateString(
                                 MessageTemplate::kWasmTrapTableOutOfBounds));
      return;
    }

    base::Vector<const uint8_t> module_bytes =
        trusted_instance_data->native_module()->wire_bytes();
    Decoder decoder(module_bytes);
    decoder.consume_bytes(elem_segment.elements_wire_bytes_offset);

    bool is_function_table =
        IsSubtypeOf(module_->tables[table_index].type, kWasmFuncRef, module_);

    if (is_function_table) {
      for (size_t i = 0; i < count; i++) {
        int entry_index = static_cast<int>(dest_offset + i);
        ValueOrError computed_element = ConsumeElementSegmentEntry(
            &init_expr_zone_, isolate_, trusted_instance_data,
            shared_trusted_instance_data, elem_segment, decoder,
            kLazyFunctionsAndNull);
        if (MaybeMarkError(computed_element, thrower_)) return;

        WasmValue computed_value = to_value(computed_element);

        if (computed_value.type() == kCanonicalI32) {
          if (computed_value.to_i32() >= 0) {
            SetFunctionTablePlaceholder(isolate_, trusted_instance_data,
                                        table_object, entry_index,
                                        computed_value.to_i32());
          } else {
            SetFunctionTableNullEntry(isolate_, table_object, entry_index);
          }
        } else {
          WasmTableObject::Set(isolate_, table_object, entry_index,
                               computed_value.to_ref());
        }
      }
    } else {
      for (size_t i = 0; i < count; i++) {
        int entry_index = static_cast<int>(dest_offset + i);
        ValueOrError computed_element = ConsumeElementSegmentEntry(
            &init_expr_zone_, isolate_, trusted_instance_data,
            shared_trusted_instance_data, elem_segment, decoder,
            kStrictFunctionsAndNull);
        if (MaybeMarkError(computed_element, thrower_)) return;
        WasmTableObject::Set(isolate_, table_object, entry_index,
                             to_value(computed_element).to_ref());
      }
    }
    // Active segment have to be set to empty after instance initialization
    // (much like passive segments after dropping).
    (elem_segment.shared ? shared_trusted_instance_data : trusted_instance_data)
        ->element_segments()
        ->set(segment_index, *isolate_->factory()->empty_fixed_array());
  }
}

void InstanceBuilder::InitializeTags(
    DirectHandle<WasmTrustedInstanceData> trusted_instance_data) {
  DirectHandle<FixedArray> tags_table(trusted_instance_data->tags_table(),
                                      isolate_);
  for (int index = 0; index < tags_table->length(); ++index) {
    if (!IsUndefined(tags_table->get(index), isolate_)) continue;
    DirectHandle<WasmExceptionTag> tag = WasmExceptionTag::New(isolate_, index);
    tags_table->set(index, *tag);
  }
}

}  // namespace v8::internal::wasm

#undef TRACE
