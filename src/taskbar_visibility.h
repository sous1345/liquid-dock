#pragma once
#include "common.h"

// Window properties survive a dock crash, so the separate recovery process can
// undo our opacity change. They disappear with Explorer's window if it restarts.
// Preserve only the layering bit/attributes; do not overwrite unrelated styles.
namespace taskbarvisibility {
constexpr auto kState = L"LiquidDock.OpacityBackup.v1";
constexpr auto kColor = L"LiquidDock.OpacityColor.v1";
constexpr ULONG_PTR kMarker = 1u << 16;
constexpr ULONG_PTR kWasLayered = 1u << 17;

inline bool Restore(HWND h) {
    ULONG_PTR saved = reinterpret_cast<ULONG_PTR>(GetPropW(h, kState));
    if (!(saved & kMarker)) return true;
    if (!IsWindow(h)) return false;
    LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (saved & kWasLayered) {
        COLORREF color = static_cast<COLORREF>(reinterpret_cast<ULONG_PTR>(GetPropW(h, kColor)));
        BYTE alpha = static_cast<BYTE>(saved & 0xff);
        DWORD flags = static_cast<DWORD>((saved >> 8) & 3);
        if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(h, GWL_EXSTYLE, style | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(h, color, alpha, flags)) return false;
        COLORREF readColor{}; BYTE readAlpha{}; DWORD readFlags{};
        if (!GetLayeredWindowAttributes(h, &readColor, &readAlpha, &readFlags) ||
            readColor != color || readAlpha != alpha || readFlags != flags) return false;
    } else {
        if (style & WS_EX_LAYERED) SetWindowLongPtrW(h, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) return false;
    }
    RemovePropW(h, kState);
    if (GetPropW(h, kState)) return false;
    RemovePropW(h, kColor);
    return true;
}

inline bool IsProtected(HWND h) {
    COLORREF color{}; BYTE alpha{}; DWORD flags{};
    return IsWindow(h) && (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) &&
           (reinterpret_cast<ULONG_PTR>(GetPropW(h, kState)) & kMarker) &&
           GetLayeredWindowAttributes(h, &color, &alpha, &flags) && alpha == 0 && (flags & LWA_ALPHA);
}

inline bool Protect(HWND h) {
    if (IsProtected(h)) return true;
    if (!IsWindow(h)) return false;
    ULONG_PTR existing = reinterpret_cast<ULONG_PTR>(GetPropW(h, kState));
    if (existing && !(existing & kMarker)) return false;
    LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (!GetPropW(h, kState)) {
        COLORREF color = 0; BYTE alpha = 255; DWORD flags = LWA_ALPHA;
        // UpdateLayeredWindow content cannot be reconstructed from these attributes.
        if ((style & WS_EX_LAYERED) && !GetLayeredWindowAttributes(h, &color, &alpha, &flags)) return false;
        ULONG_PTR saved = kMarker | ((style & WS_EX_LAYERED) ? kWasLayered : 0) | (flags << 8) | alpha;
        if (!SetPropW(h, kColor, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(color)))) return false;
        if (!SetPropW(h, kState, reinterpret_cast<HANDLE>(saved))) { RemovePropW(h, kColor); return false; }
    }
    if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(h, GWL_EXSTYLE, style | WS_EX_LAYERED);
    if (!SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA) || !IsProtected(h)) {
        Restore(h); // failed rollback retains its marker for the watchdog to retry
        return false;
    }
    return true;
}
}
