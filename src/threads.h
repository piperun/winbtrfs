/* Copyright (c) Mark Harmstone 2016-17
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#pragma once

uint32_t get_num_of_processors();
NTSTATUS create_calc_threads(_In_ PDEVICE_OBJECT DeviceObject);
void stop_calc_threads(_In_ device_extension* Vcb);
void stop_flush_thread(_In_ device_extension* Vcb);

_Function_class_(KSTART_ROUTINE)
void __stdcall calc_thread(void* context);

_Function_class_(KSTART_ROUTINE)
void __stdcall flush_thread(void* context);
