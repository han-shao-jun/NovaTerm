"""Print the exception and faulting module from a Windows x64 minidump."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def unpack(fmt: str, data: bytes, offset: int):
    return struct.unpack_from(fmt, data, offset)


dump_path = Path(sys.argv[1])
data = dump_path.read_bytes()
signature, _, stream_count, directory_rva, _, _, flags = unpack("<IIIIIIQ", data, 0)
if signature != 0x504D444D:
    raise SystemExit(f"{dump_path} is not a minidump")

streams: dict[int, tuple[int, int]] = {}
for index in range(stream_count):
    stream_type, size, rva = unpack("<III", data, directory_rva + index * 12)
    streams[stream_type] = (size, rva)

_, exception_rva = streams[6]
thread_id = unpack("<I", data, exception_rva)[0]
exception_code, exception_flags = unpack("<II", data, exception_rva + 8)
exception_address = unpack("<Q", data, exception_rva + 24)[0]
parameter_count = unpack("<I", data, exception_rva + 32)[0]
parameters = unpack(f"<{parameter_count}Q", data, exception_rva + 40) if parameter_count else ()
context_size, context_rva = unpack("<II", data, exception_rva + 160)
stack_pointer = unpack("<Q", data, context_rva + 152)[0]
instruction_pointer = unpack("<Q", data, context_rva + 248)[0]
register_names = (
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
)
registers = unpack("<16Q", data, context_rva + 120)

print(f"file: {dump_path}")
print(f"flags: {flags:#x}")
print(f"thread: {thread_id}")
print(f"exception: {exception_code:#010x} flags={exception_flags:#x}")
print(f"exception_address: {exception_address:#x}")
print("exception_parameters:", " ".join(f"{value:#x}" for value in parameters))
print(f"rip: {instruction_pointer:#x}")
print(f"rsp: {stack_pointer:#x}")
print("registers:", " ".join(
    f"{name}={value:#x}" for name, value in zip(register_names, registers)
))
print(f"context: size={context_size} rva={context_rva:#x}")

_, module_list_rva = streams[4]
module_count = unpack("<I", data, module_list_rva)[0]
print(f"modules: {module_count}")
modules = []
for index in range(module_count):
    module_rva = module_list_rva + 4 + index * 108
    base, image_size, _, timestamp, name_rva = unpack("<QIIII", data, module_rva)
    name_size = unpack("<I", data, name_rva)[0]
    name = data[name_rva + 4 : name_rva + 4 + name_size].decode("utf-16le", "replace")
    modules.append((base, base + image_size, name))
    if base <= instruction_pointer < base + image_size:
        print(f"fault_module: {name}")
        print(f"module_base: {base:#x}")
        print(f"module_size: {image_size:#x}")
        print(f"module_timestamp: {timestamp:#x}")
        print(f"module_offset: {instruction_pointer - base:#x}")

if 5 in streams:
    _, memory_list_rva = streams[5]
    memory_count = unpack("<I", data, memory_list_rva)[0]
    for index in range(memory_count):
        descriptor_rva = memory_list_rva + 4 + index * 16
        start, size, content_rva = unpack("<QII", data, descriptor_rva)
        if start <= stack_pointer < start + size:
            stack_offset = content_rva + stack_pointer - start
            stack_data = data[stack_offset : content_rva + size]
            print("probable_stack_returns:")
            for offset in range(0, len(stack_data) - 7, 8):
                value = unpack("<Q", stack_data, offset)[0]
                for base, end, name in modules:
                    if base <= value < end:
                        print(f"  {stack_pointer + offset:#x}: {name} + {value - base:#x}")
                        break
