#pragma once

#include <cstddef>

void* FindVirtualTable(const char* moduleSuffix, const char* className);
void* FindFunctionSignature(const char* moduleSuffix, const char* sectionName, const char* idaSig);
/// Tries signatures in order; stops at first hit. idaSigs is nullptr-terminated.
void* FindFunctionSignatureAny(const char* moduleSuffix, const char* sectionName, const char* const* idaSigs);
