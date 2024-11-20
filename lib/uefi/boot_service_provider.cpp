/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include "boot_service_provider.h"
#include "boot_service.h"
#include "kernel/vm.h"
#include "lib/dlmalloc.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vmm_aspace_t *old_aspace = nullptr;

vmm_aspace_t *set_boot_aspace() {
  static vmm_aspace_t *aspace = nullptr;
  if (aspace == nullptr) {
    auto err = vmm_create_aspace(&aspace, "linux_kernel", 0);
    if (err) {
      printf("Failed to create address space for linux kernel %d\n", err);
      return nullptr;
    }
    old_aspace = vmm_set_active_aspace(aspace);
  }
  return aspace;
}

void restore_aspace() { vmm_set_active_aspace(old_aspace); }

void *identity_map(void *addr, size_t size) {
  size = ROUNDUP(size, PAGE_SIZE);
  auto vaddr = reinterpret_cast<vaddr_t>(addr);
  paddr_t pa{};
  uint flags{};
  auto aspace = set_boot_aspace();
  auto err = arch_mmu_query(&aspace->arch_aspace, vaddr, &pa, &flags);
  if (err) {
    printf("Failed to query physical address for memory 0x%p\n", addr);
    return nullptr;
  }

  err = arch_mmu_unmap(&aspace->arch_aspace, vaddr, size / PAGE_SIZE);
  if (err) {
    printf("Failed to unmap virtual address 0x%lx\n", vaddr);
    return nullptr;
  }
  arch_mmu_map(&aspace->arch_aspace, pa, pa, size / PAGE_SIZE, flags);
  if (err) {
    printf("Failed to identity map physical address 0x%lx\n", pa);
    return nullptr;
  }
  printf("Identity mapped physical address 0x%lx flags 0x%x\n", pa, flags);

  return reinterpret_cast<void *>(pa);
}

void *alloc_page(size_t size, size_t align_log2) {
  auto aspace = set_boot_aspace();
  void *vptr{};
  status_t err = vmm_alloc_contiguous(aspace, "uefi_program", size, &vptr,
                                      align_log2, 0, 0);
  if (err) {
    printf("Failed to allocate memory for uefi program %d\n", err);
    return nullptr;
  }
  return identity_map(vptr, size);
}

void *alloc_page(void *addr, size_t size, size_t align_log2) {
  if (addr == nullptr) {
    return alloc_page(size, align_log2);
  }
  auto err =
      vmm_alloc_contiguous(set_boot_aspace(), "uefi_program", size, &addr,
                           align_log2, VMM_FLAG_VALLOC_SPECIFIC, 0);
  if (err) {
    printf(
        "Failed to allocate memory for uefi program @ fixed address 0x%p %d , "
        "falling back to non-fixed allocation\n",
        addr, err);
    return alloc_page(size, align_log2);
  }
  return identity_map(addr, size);
}

namespace {

EfiStatus unload(EfiHandle handle) { return SUCCESS; }

bool guid_eq(const EfiGuid *a, const EfiGuid *b) {
  return memcmp(a, b, sizeof(*a)) == 0;
}

bool guid_eq(const EfiGuid *a, const EfiGuid &b) {
  return memcmp(a, &b, sizeof(*a)) == 0;
}

EfiStatus handle_protocol(EfiHandle handle, const EfiGuid *protocol,
                          void **intf) {
  if (guid_eq(protocol, LOADED_IMAGE_PROTOCOL_GUID)) {
    printf("handle_protocol(%p, LOADED_IMAGE_PROTOCOL_GUID, %p);\n", handle,
           intf);
    auto loaded_image = static_cast<EFI_LOADED_IMAGE_PROTOCOL *>(
        malloc(sizeof(EFI_LOADED_IMAGE_PROTOCOL)));
    loaded_image = {};
    loaded_image->Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    loaded_image->ParentHandle = nullptr;
    loaded_image->SystemTable = nullptr;
    loaded_image->LoadOptionsSize = 0;
    loaded_image->LoadOptions = nullptr;
    loaded_image->Unload = unload;
    loaded_image->ImageBase = handle;

    *intf = loaded_image;
    return SUCCESS;
  } else if (guid_eq(protocol, LINUX_EFI_LOADED_IMAGE_FIXED_GUID)) {
    printf("handle_protocol(%p, LINUX_EFI_LOADED_IMAGE_FIXED_GUID, %p);\n",
           handle, intf);
    return SUCCESS;
  } else {
    printf("handle_protocol(%p, %p, %p);\n", handle, protocol, intf);
  }
  return UNSUPPORTED;
}

constexpr size_t kHeapSize = 8ul * 1024 * 1024;
void *get_heap() {
  static auto heap = alloc_page(kHeapSize);
  return heap;
}

mspace create_mspace_with_base_limit(void *base, size_t capacity, int locked) {
  auto space = create_mspace_with_base(get_heap(), kHeapSize, 1);
  mspace_set_footprint_limit(space, capacity);
  return space;
}

mspace get_mspace() {
  static auto space = create_mspace_with_base_limit(get_heap(), kHeapSize, 1);
  return space;
}

EfiStatus allocate_pool(EfiMemoryType pool_type, size_t size, void **buf) {
  if (buf == nullptr) {
    return INVALID_PARAMETER;
  }
  if (size == 0) {
    *buf = nullptr;
    return SUCCESS;
  }
  *buf = mspace_malloc(get_mspace(), size);
  if (*buf != nullptr) {
    return SUCCESS;
  }
  return OUT_OF_RESOURCES;
}

EfiStatus free_pool(void *mem) {
  mspace_free(get_mspace(), mem);
  return SUCCESS;
}

size_t get_aspace_entry_count(vmm_aspace_t *aspace) {
  vmm_region_t *region = nullptr;
  size_t num_entries = 0;
  list_for_every_entry(&aspace->region_list, region, vmm_region_t, node) {
    num_entries++;
  }
  return num_entries;
}

void fill_memory_map_entry(vmm_aspace_t *aspace, EfiMemoryDescriptor *entry,
                           const vmm_region_t *region) {
  entry->virtual_start = region->base;
  entry->physical_start = entry->virtual_start;
  entry->number_of_pages = region->size / PAGE_SIZE;
  paddr_t pa{};
  uint flags{};
  status_t err =
      arch_mmu_query(&aspace->arch_aspace, region->base, &pa, &flags);
  if (err >= 0) {
    entry->physical_start = pa;
  }
  if ((flags & ARCH_MMU_FLAG_CACHE_MASK) == ARCH_MMU_FLAG_CACHED) {
    entry->attributes |= EFI_MEMORY_WB | EFI_MEMORY_WC | EFI_MEMORY_WT;
  }
}

EfiStatus get_physical_memory_map(size_t *memory_map_size,
                                  EfiMemoryDescriptor *memory_map,
                                  size_t *map_key, size_t *desc_size,
                                  uint32_t *desc_version) {
  if (memory_map_size == nullptr) {
    return INVALID_PARAMETER;
  }
  if (map_key) {
    *map_key = 0;
  }
  if (desc_size) {
    *desc_size = sizeof(EfiMemoryDescriptor);
  }
  if (desc_version) {
    *desc_version = 1;
  }
  pmm_arena_t *a{};
  size_t num_entries = 0;
  list_for_every_entry(get_arena_list(), a, pmm_arena_t, node) {
    num_entries++;
  }
  const size_t size_needed = num_entries * sizeof(EfiMemoryDescriptor);
  if (*memory_map_size < size_needed) {
    *memory_map_size = size_needed;
    return BUFFER_TOO_SMALL;
  }
  *memory_map_size = size_needed;
  size_t i = 0;
  memset(memory_map, 0, size_needed);
  list_for_every_entry(get_arena_list(), a, pmm_arena_t, node) {
    memory_map[i].physical_start = a->base;
    memory_map[i].number_of_pages = a->size / PAGE_SIZE;
    memory_map[i].attributes |= EFI_MEMORY_WB;
    memory_map[i].memory_type = LOADER_CODE;
    i++;
  }
  return SUCCESS;
}

EfiStatus get_memory_map(size_t *memory_map_size,
                         EfiMemoryDescriptor *memory_map, size_t *map_key,
                         size_t *desc_size, uint32_t *desc_version) {
  if (memory_map_size == nullptr) {
    return INVALID_PARAMETER;
  }
  if (map_key) {
    *map_key = 0;
  }
  if (desc_size) {
    *desc_size = sizeof(EfiMemoryDescriptor);
  }
  if (desc_version) {
    *desc_version = 1;
  }
  vmm_region_t *region = nullptr;
  auto aspace = vmm_get_kernel_aspace();
  size_t num_entries = 0;
  list_for_every_entry(&aspace->region_list, region, vmm_region_t, node) {
    num_entries++;
  }
  const size_t size_needed = num_entries * sizeof(EfiMemoryDescriptor);
  if (*memory_map_size < size_needed) {
    *memory_map_size = size_needed;
    return BUFFER_TOO_SMALL;
  }
  *memory_map_size = size_needed;
  size_t i = 0;
  memset(memory_map, 0, size_needed);
  list_for_every_entry(&aspace->region_list, region, vmm_region_t, node) {
    memory_map[i].virtual_start = region->base;
    memory_map[i].physical_start = memory_map[i].virtual_start;
    memory_map[i].number_of_pages = region->size / PAGE_SIZE;
    paddr_t pa{};
    uint flags{};
    status_t err =
        arch_mmu_query(&aspace->arch_aspace, region->base, &pa, &flags);
    if (err >= 0) {
      memory_map[i].physical_start = pa;
    }
    i++;
  }

  return SUCCESS;
}

EfiStatus register_protocol_notify(const EfiGuid *protocol, EfiEvent event,
                                   void **registration) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus locate_handle(EfiLocateHandleSearchType search_type,
                        const EfiGuid *protocol, void *search_key,
                        size_t *buf_size, EfiHandle *buf) {

  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus locate_protocol(const EfiGuid *protocol, void *registration,
                          void **intf) {
  if (protocol == nullptr) {
    return INVALID_PARAMETER;
  }
  if (memcmp(protocol, &EFI_RNG_PROTOCOL_GUID, sizeof(*protocol)) == 0) {
    printf("%s(EFI_RNG_PROTOCOL_GUID) is unsupported.\n", __FUNCTION__);
    return UNSUPPORTED;
  }
  if (memcmp(protocol, &EFI_TCG2_PROTOCOL_GUID, sizeof(*protocol)) == 0) {
    printf("%s(EFI_TCG2_PROTOCOL_GUID) is unsupported.\n", __FUNCTION__);
    return NOT_FOUND;
  }

  printf("%s(%x %x %x %llx) is unsupported\n", __FUNCTION__, protocol->data1,
         protocol->data2, protocol->data3,
         *reinterpret_cast<const uint64_t *>(&protocol->data4));
  return UNSUPPORTED;
}

EfiStatus allocate_pages(EfiAllocatorType type, EfiMemoryType memory_type,
                         size_t pages, EfiPhysicalAddr *memory) {
  if (memory == nullptr) {
    return INVALID_PARAMETER;
  }
  if (type == ALLOCATE_MAX_ADDRESS && *memory != 0xffffffffffffffff) {
    printf("allocate_pages(%d, %d, %zu, 0x%llx) unsupported\n", type,
           memory_type, pages, *memory);
    return UNSUPPORTED;
  }
  *memory = reinterpret_cast<EfiPhysicalAddr>(alloc_page(pages * PAGE_SIZE));
  if (*memory == 0) {
    return OUT_OF_RESOURCES;
  }
  return SUCCESS;
}

EfiStatus free_pages(EfiPhysicalAddr memory, size_t pages) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus uninstall_multiple_protocol_interfaces(EfiHandle handle, ...) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}
EfiStatus calculate_crc32(void *data, size_t len, uint32_t *crc32) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus uninstall_protocol_interface(EfiHandle handle,
                                       const EfiGuid *protocol, void *intf) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus load_image(bool boot_policy, EfiHandle parent_image_handle,
                     EfiDevicePathProtocol *path, void *src, size_t src_size,
                     EfiHandle *image_handle) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus locate_device_path(const EfiGuid *protocol,
                             EfiDevicePathProtocol **path, EfiHandle *device) {
  if (memcmp(protocol, &EFI_LOAD_FILE2_PROTOCOL_GUID,
             sizeof(EFI_LOAD_FILE2_PROTOCOL_GUID)) == 0) {
    return NOT_FOUND;
  }
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus install_configuration_table(const EfiGuid *guid, void *table) {
  printf("%s is unsupported\n", __FUNCTION__);
  return UNSUPPORTED;
}

EfiStatus exit_boot_services(EfiHandle image_handle, size_t map_key) {
  printf("%s is called\n", __FUNCTION__);
  return SUCCESS;
}

} // namespace

void setup_boot_service_table(EfiBootService *service) {
  service->handle_protocol = handle_protocol;
  service->allocate_pool = allocate_pool;
  service->free_pool = free_pool;
  service->get_memory_map = get_physical_memory_map;
  service->register_protocol_notify = register_protocol_notify;
  service->locate_handle = locate_handle;
  service->locate_protocol = locate_protocol;
  service->allocate_pages = allocate_pages;
  service->free_pages = free_pages;
  service->uninstall_multiple_protocol_interfaces =
      uninstall_multiple_protocol_interfaces;
  service->calculate_crc32 = calculate_crc32;
  service->uninstall_protocol_interface = uninstall_protocol_interface;
  service->load_image = load_image;
  service->locate_device_path = locate_device_path;
  service->install_configuration_table = install_configuration_table;
  service->exit_boot_services = exit_boot_services;
}
