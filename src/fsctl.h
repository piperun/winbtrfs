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

NTSTATUS fsctl_request(PDEVICE_OBJECT DeviceObject, PIRP* Pirp, uint32_t type);
void do_unlock_volume(device_extension* Vcb);
void trim_whole_device(device* dev);
void flush_subvol_fcbs(root* subvol);
bool fcb_is_inline(fcb* fcb);
NTSTATUS dismount_volume(device_extension* Vcb, bool shutdown, PIRP Irp);
