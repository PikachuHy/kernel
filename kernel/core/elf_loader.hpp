#pragma once
#include <stdint.h>
#include <stddef.h>

class Process;
struct Thread;

// Load an ELF64 executable from a memory buffer. Creates a Process,
// maps PT_LOAD segments as VMOs, creates a main Thread with the ELF
// entry point. The thread is NOT started -- caller must call thread_start.
// Returns the Process on success, or nullptr on failure.
// On success, *out_thread points to the created (not-yet-started) Thread.
Process* elf_load(const void* elf_data, size_t elf_size,
                  const char* proc_name, uint8_t priority,
                  Thread** out_thread);

// Load and start the embedded init process.
// Defined in elf_loader.cpp, references the init_embed object symbols.
extern "C" void elf_load_init_process();
