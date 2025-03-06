// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Generate this file using the update-object-macros-undef.py script.

// PRESUBMIT_INTENTIONALLY_MISSING_INCLUDE_GUARD

#undef V8_OBJECT_MACROS_DEFINED
#undef V8_OBJECT_PUSH
#undef V8_OBJECT_POP
#undef V8_OBJECT
#undef V8_OBJECT_END
#undef V8_OBJECT_INNER_CLASS
#undef V8_OBJECT_INNER_CLASS_END
#undef OBJECT_CONSTRUCTORS
#undef OBJECT_CONSTRUCTORS_IMPL
#undef NEVER_READ_ONLY_SPACE
#undef NEVER_READ_ONLY_SPACE_IMPL
#undef DECL_PRIMITIVE_GETTER
#undef DECL_PRIMITIVE_SETTER
#undef DECL_PRIMITIVE_ACCESSORS
#undef DECL_BOOLEAN_ACCESSORS
#undef DECL_INT_ACCESSORS
#undef DECL_INT32_ACCESSORS
#undef DECL_SANDBOXED_POINTER_ACCESSORS
#undef DECL_UINT16_ACCESSORS
#undef DECL_INT16_ACCESSORS
#undef DECL_UINT8_ACCESSORS
#undef DECL_RELAXED_PRIMITIVE_ACCESSORS
#undef DECL_RELAXED_INT32_ACCESSORS
#undef DECL_RELAXED_UINT32_ACCESSORS
#undef DECL_RELAXED_UINT16_ACCESSORS
#undef DECL_RELAXED_UINT8_ACCESSORS
#undef DECL_GETTER
#undef DEF_GETTER
#undef DEF_RELAXED_GETTER
#undef DEF_ACQUIRE_GETTER
#undef DEF_HEAP_OBJECT_PREDICATE
#undef TQ_FIELD_TYPE
#undef DECL_FIELD_OFFSET_TQ
#undef DECL_SETTER
#undef DECL_ACCESSORS
#undef DECL_ACCESSORS_LOAD_TAG
#undef DECL_ACCESSORS_STORE_TAG
#undef DECL_RELAXED_GETTER
#undef DECL_RELAXED_SETTER
#undef DECL_RELAXED_ACCESSORS
#undef DECL_ACQUIRE_GETTER
#undef DECL_RELEASE_SETTER
#undef DECL_RELEASE_ACQUIRE_ACCESSORS
#undef DEF_PRIMITIVE_ACCESSORS
#undef INT_ACCESSORS
#undef INT32_ACCESSORS
#undef UINT16_ACCESSORS
#undef UINT8_ACCESSORS
#undef RELAXED_INT32_ACCESSORS
#undef RELAXED_UINT32_ACCESSORS
#undef RELAXED_UINT16_ACCESSORS
#undef RELAXED_UINT8_ACCESSORS
#undef ACCESSORS_CHECKED2
#undef ACCESSORS_CHECKED
#undef ACCESSORS
#undef ACCESSORS_NOCAGE
#undef RENAME_TORQUE_ACCESSORS
#undef RENAME_PRIMITIVE_TORQUE_ACCESSORS
#undef ACCESSORS_RELAXED_CHECKED2
#undef ACCESSORS_RELAXED_CHECKED
#undef ACCESSORS_RELAXED
#undef RELAXED_ACCESSORS_CHECKED2
#undef RELAXED_ACCESSORS_CHECKED
#undef RELAXED_ACCESSORS
#undef RELEASE_ACQUIRE_GETTER_CHECKED
#undef RELEASE_ACQUIRE_SETTER_CHECKED
#undef RELEASE_ACQUIRE_ACCESSORS_CHECKED2
#undef RELEASE_ACQUIRE_ACCESSORS_CHECKED
#undef RELEASE_ACQUIRE_ACCESSORS
#undef SMI_ACCESSORS_CHECKED
#undef SMI_ACCESSORS
#undef DECL_RELEASE_ACQUIRE_INT_ACCESSORS
#undef RELEASE_ACQUIRE_SMI_ACCESSORS
#undef DECL_RELAXED_INT_ACCESSORS
#undef RELAXED_SMI_ACCESSORS
#undef BOOL_GETTER
#undef BOOL_ACCESSORS
#undef DECL_RELAXED_BOOL_ACCESSORS
#undef RELAXED_BOOL_ACCESSORS
#undef DECL_EXTERNAL_POINTER_ACCESSORS_MAYBE_READ_ONLY_HOST
#undef EXTERNAL_POINTER_ACCESSORS_MAYBE_READ_ONLY_HOST
#undef DECL_EXTERNAL_POINTER_ACCESSORS
#undef EXTERNAL_POINTER_ACCESSORS
#undef DECL_TRUSTED_POINTER_GETTERS
#undef DECL_TRUSTED_POINTER_SETTERS
#undef DECL_TRUSTED_POINTER_ACCESSORS
#undef TRUSTED_POINTER_ACCESSORS
#undef DECL_CODE_POINTER_ACCESSORS
#undef CODE_POINTER_ACCESSORS
#undef DECL_PROTECTED_POINTER_ACCESSORS
#undef PROTECTED_POINTER_ACCESSORS
#undef DECL_RELEASE_ACQUIRE_PROTECTED_POINTER_ACCESSORS
#undef RELEASE_ACQUIRE_PROTECTED_POINTER_ACCESSORS
#undef BIT_FIELD_ACCESSORS2
#undef BIT_FIELD_ACCESSORS
#undef RELAXED_INT16_ACCESSORS
#undef FIELD_ADDR
#undef SEQ_CST_READ_FIELD
#undef ACQUIRE_READ_FIELD
#undef RELAXED_READ_FIELD
#undef RELAXED_READ_WEAK_FIELD
#undef WRITE_FIELD
#undef SEQ_CST_WRITE_FIELD
#undef RELEASE_WRITE_FIELD
#undef RELAXED_WRITE_FIELD
#undef RELAXED_WRITE_WEAK_FIELD
#undef SEQ_CST_SWAP_FIELD
#undef SEQ_CST_COMPARE_AND_SWAP_FIELD
#undef WRITE_BARRIER
#undef EXTERNAL_POINTER_WRITE_BARRIER
#undef INDIRECT_POINTER_WRITE_BARRIER
#undef JS_DISPATCH_HANDLE_WRITE_BARRIER
#undef CONDITIONAL_WRITE_BARRIER
#undef CONDITIONAL_EXTERNAL_POINTER_WRITE_BARRIER
#undef CONDITIONAL_INDIRECT_POINTER_WRITE_BARRIER
#undef CONDITIONAL_TRUSTED_POINTER_WRITE_BARRIER
#undef CONDITIONAL_CODE_POINTER_WRITE_BARRIER
#undef CONDITIONAL_PROTECTED_POINTER_WRITE_BARRIER
#undef CONDITIONAL_JS_DISPATCH_HANDLE_WRITE_BARRIER
#undef ACQUIRE_READ_INT8_FIELD
#undef ACQUIRE_READ_INT32_FIELD
#undef RELAXED_WRITE_INT8_FIELD
#undef RELAXED_READ_INT8_FIELD
#undef RELAXED_WRITE_UINT8_FIELD
#undef RELAXED_READ_UINT8_FIELD
#undef RELAXED_READ_UINT16_FIELD
#undef RELAXED_WRITE_UINT16_FIELD
#undef RELAXED_READ_INT16_FIELD
#undef RELAXED_WRITE_INT16_FIELD
#undef RELAXED_READ_UINT32_FIELD
#undef ACQUIRE_READ_UINT32_FIELD
#undef RELAXED_WRITE_UINT32_FIELD
#undef RELEASE_WRITE_INT8_FIELD
#undef RELEASE_WRITE_UINT32_FIELD
#undef RELAXED_READ_INT32_FIELD
#undef RELAXED_READ_INT64_FIELD
#undef RELEASE_WRITE_INT32_FIELD
#undef RELAXED_WRITE_INT32_FIELD
#undef RELAXED_READ_INT_FIELD
#undef RELAXED_WRITE_INT_FIELD
#undef RELAXED_READ_UINT_FIELD
#undef RELAXED_WRITE_UINT_FIELD
#undef RELAXED_READ_BYTE_FIELD
#undef ACQUIRE_READ_BYTE_FIELD
#undef RELAXED_WRITE_BYTE_FIELD
#undef RELEASE_WRITE_BYTE_FIELD
#undef DECL_PRINTER
#undef DECL_VERIFIER
#undef EXPORT_DECL_VERIFIER
#undef DECL_STATIC_VERIFIER
#undef EXPORT_DECL_STATIC_VERIFIER
#undef DEFINE_DEOPT_ELEMENT_ACCESSORS
#undef DEFINE_DEOPT_ENTRY_ACCESSORS
#undef TQ_OBJECT_CONSTRUCTORS
#undef TQ_OBJECT_CONSTRUCTORS_IMPL
#undef TQ_CPP_OBJECT_DEFINITION_ASSERTS
