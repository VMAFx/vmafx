// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

use vmafx::Status;

#[test]
fn every_status_round_trips() {
    let cases = [
        (Status::Ok, 0, "VMAFX_OK", "0"),
        (Status::Pending, 1, "VMAFX_PENDING", "EAGAIN"),
        (Status::EInvalid, -1, "VMAFX_E_INVALID", "EINVAL"),
        (Status::ENoMem, -2, "VMAFX_E_NOMEM", "ENOMEM"),
        (Status::ENotSup, -3, "VMAFX_E_NOTSUP", "ENOTSUP"),
        (Status::ENotFound, -4, "VMAFX_E_NOTFOUND", "ENOENT"),
        (Status::EBusy, -5, "VMAFX_E_BUSY", "EBUSY"),
        (Status::EDevice, -6, "VMAFX_E_DEVICE", "EIO"),
        (Status::EIo, -7, "VMAFX_E_IO", "EIO"),
        (Status::ERange, -8, "VMAFX_E_RANGE", "ERANGE"),
        (Status::EInternal, -9, "VMAFX_E_INTERNAL", "EIO"),
        (Status::ETimeout, -10, "VMAFX_E_TIMEOUT", "ETIMEDOUT"),
        (Status::EAbi, -11, "VMAFX_E_ABI", "EINVAL"),
        (Status::EMismatch, -12, "VMAFX_E_MISMATCH", "EBADMSG"),
    ];
    for (status, raw, name, errno) in cases {
        assert_eq!(status as i32, raw);
        assert_eq!(Status::from_raw(raw), Some(status));
        assert_eq!(status.as_str(), name);
        assert_eq!(status.errno(), errno);
    }
    assert_eq!(std::mem::size_of::<Status>(), std::mem::size_of::<i32>());
}

#[test]
fn unknown_codes_are_rejected() {
    for raw in [i32::MIN, -13, 2, i32::MAX] {
        assert_eq!(Status::from_raw(raw), None);
    }
}
