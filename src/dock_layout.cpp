#include "dock.h"
#include "system.h"

// ---------------------------------------------------------------- layout

IDWriteTextFormat* Dock::ClockFont(bool date) const {
    if (date) return g_settings.clockSize == 1 ? g_gfx.fmtDateLarge.Get() : g_gfx.fmtDate.Get();
    return g_settings.clockSize == 1 ? g_gfx.fmtTimeLarge.Get() : g_gfx.fmtTime.Get();
}

float Dock::SmallButtonWidth(const SmallButton& b, const LayoutMetrics& m) const {
    return Shown(b) ? std::max(m.icon * b.iconFactor, b.minWidth * scale) : 0;
}

// Lays out one group of small buttons from x, in table order. When stacking, two neighbours share
// a cell (an odd one keeps a whole cell); hidden buttons take no space. Returns the end of the group.
float Dock::ArrangeGroup(int group, const LayoutMetrics& m, float x, bool assign) {
    Cell* waiting = nullptr; float waitingW = 0;
    for (const SmallButton* b = kSmallButtons; b->kind != HitNone; ++b) {
        if (b->group != group) continue;
        Cell scratch, &c = assign ? cells[b->kind] : scratch;
        float w = SmallButtonWidth(*b, m);
        c.half = -1;
        if (w <= 0) { c.u0 = c.u1 = x; continue; }
        if (!m.stack) { c.u0 = x; c.u1 = x += w; continue; }
        if (!waiting) { waiting = &c; waitingW = w; c.u0 = x; c.u1 = x + w; continue; }
        float cell = std::max(waitingW, w);
        if (assign) { waiting->u1 = x + cell; waiting->half = 0; }
        c.u0 = x; c.u1 = x + cell; c.half = 1;
        x += cell; waiting = nullptr;
    }
    return waiting ? x + waitingW : x;
}

Dock::LayoutMetrics Dock::MeasureMetrics(float fitFactor, int thicknessPct) {
    LayoutMetrics m{};
    constexpr float spacing[] = { 0.22f, 0.32f, 0.44f };
    // icons are 60% of the default glass thickness; the thickness setting only changes the padding
    float pct = (thicknessPct > 0 ? thicknessPct : std::clamp(g_settings.dockThickness, 75, 150)) / 100.0f;
    m.icon = std::max(1.0f, std::round(baseNominal * fitFactor * 1.30f * 0.60f));
    float thick = m.icon / 0.60f * pct;
    m.gap = std::round(m.icon * spacing[std::clamp(g_settings.iconSpacing, 0, 2)]);

    bool vert = IsVertical(g_settings.edge);
    const auto time = TextSize(ClockFont(), vert ? L"88:88" : g_sys.timeText);
    const auto date = TextSize(ClockFont(true), g_sys.dateText);
    float clockDepth = vert ? time.width : time.height + date.height;
    // Keep the clock readable even when many apps require smaller icons. The
    // extra space is padding; clock size does not change the icon-size setting.
    m.padding = std::ceil(std::max((thick - m.icon) * 0.5f,
                                   (clockDepth + 8 * scale - m.icon) * 0.5f));
    m.thickness = m.icon + 2 * m.padding;
    m.separator = 1 + m.gap;
    m.trayButton = m.icon * 0.45f;
    // pair the small buttons while half the bar still makes a comfortable target
    m.stack = m.thickness * 0.5f >= 22 * scale;
    m.clock = vert ? std::max(m.icon * 0.8f, time.height + 8 * scale)
                   : std::max(time.width, date.width) + 12 * scale;
    m.sensor = vert ? m.icon * 1.3f
                    : TextWidth(g_gfx.fmtClockSmall.Get(), L"CPU 100°") + 16 * scale;
    m.connectivity = ArrangeGroup(0, m, 0, false);
    m.utilities = ArrangeGroup(1, m, 0, false);
    return m;
}

void Dock::ApplyFit(float dt) {
    // Use the same measured tray and spacing as Layout, including the clock
    // and optional desktop button. Fit never depends on the cursor position.
    if (dt > 0 && len > 0) {
        float n = 0;
        for (auto& s : slots) n += clampf(s.appear.target, 0, 1);
        auto required = [&](float factor) {
            const auto m = MeasureMetrics(factor);
            // Upper bound for the cosine expansion budget on equally spaced
            // slots. Reserve it before hover so a full dock cannot jump in size.
            float expansion = m.icon * (mag - 1) *
                std::min(n, 1 + m.icon * 2.9f / (m.icon + m.gap));
            return 2 * m.padding + n * (m.icon + m.gap) + m.TrayWidth() + expansion;
        };
        float target = 1;
        const float avail = len - 24 * scale;
        if (required(1) > avail) {
            // The user prefers further icon reduction to losing items off-screen.
            // Keep a physical-pixel floor; the clock retains its selected font.
            float lo = 0, hi = 1;
            for (int i = 0; i < 16; ++i) {
                float mid = (lo + hi) * 0.5f;
                if (required(mid) <= avail) lo = mid; else hi = mid;
            }
            target = lo;
        }
        float k = std::min(1.0f, dt * 10);
        fit += (target - fit) * k;
        if (std::fabs(target - fit) < 0.001f) fit = target;
    }
    const auto m = MeasureMetrics(fit);
    base = m.icon; pad = m.padding; gap = m.gap; barThick = m.thickness;
    magRange = base * 2.9f;
}

void Dock::Layout(double) {
    float unit = base + gap;
    const auto metrics = MeasureMetrics(fit);
    trayW = metrics.TrayWidth();

    std::vector<float> c0(slots.size());
    float x = pad;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        s.weight = clampf(s.appear.x, 0, 1.2f) * clampf(s.shrink.x, 0, 1);
        x += s.gapBefore.x * unit;
        c0[i] = x + base * s.weight * 0.5f;
        x += unit * s.weight;
    }
    x += trailingGap.x * unit;
    float total0 = x + trayW + pad;
    // edgeOffset slides the dock along the edge (-100 / 100: against either end). The shift is
    // fixed from the resting length minus the room magnification can take (as reserved by
    // ApplyFit), so the dock still grows evenly to both sides and never runs into the screen end.
    float weights = 0;
    for (auto& s : slots) weights += s.weight;
    edgeReserve = mag > 1.001f ? base * (mag - 1) * std::min(weights, 1 + base * 2.9f / (base + gap)) : 0;
    float off = std::clamp(g_settings.edgeOffset, -100, 100) / 100.0f;
    float shift = std::max(0.0f, len - total0 - edgeReserve - 8 * scale) * 0.5f * off;
    float start0 = std::round(len * 0.5f - total0 * 0.5f + shift);

    // Keep the expansion budget independent of cursor position so that the dock
    // stays centered and its ends do not slide while the pointer moves along it.
    std::vector<float> sz(slots.size(), base);
    float m = magAmount.x;
    if (m > 0.0005f && mag > 1.001f && !c0.empty()) {
        float hu = clampf(hoverU - start0, c0.front(), c0.back());
        auto bell = [](float distance, float radius) {
            return distance < radius ? 0.5f * (1 + std::cos(kPi * distance / radius)) : 0.0f;
        };
        auto weightSum = [&](float center, float radius) {
            float sum = 0;
            for (size_t i = 0; i < slots.size(); ++i)
                sum += slots[i].weight * bell(std::fabs(center - c0[i]), radius);
            return sum;
        };
        float budget = weightSum((c0.front() + c0.back()) * 0.5f, magRange);
        float lo = 0.001f, hi = magRange;
        for (int i = 0; i < 16 && weightSum(hu, hi) < budget; ++i) hi *= 2;
        // Near either end, widen the bell toward the remaining icons rather than
        // translating the whole dock or adding empty padding beside the tray.
        for (int i = 0; i < 14; ++i) {
            float mid = (lo + hi) * 0.5f;
            if (weightSum(hu, mid) < budget) lo = mid; else hi = mid;
        }
        float sum = weightSum(hu, hi);
        float correction = sum > 0 ? std::min(1.0f, budget / sum) : 0;
        for (size_t i = 0; i < slots.size(); ++i) {
            float f = bell(std::fabs(hu - c0[i]), hi) * correction;
            sz[i] = base * (1 + (mag - 1) * f * m);
        }
    }
    float xm = pad, total;
    for (size_t i = 0; i < slots.size(); ++i) xm += slots[i].gapBefore.x * unit + (sz[i] + gap) * slots[i].weight;
    xm += trailingGap.x * unit;
    total = xm + trayW + pad;

    float start = (len - total) * 0.5f + shift;
    if (total < len - 8 * scale) start = clampf(start, 4 * scale, len - 4 * scale - total);

    x = start + pad;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        x += s.gapBefore.x * unit;
        s.u0 = x;
        s.size = sz[i];
        s.u = x + sz[i] * s.weight * 0.5f;
        x += (sz[i] + gap) * s.weight;
    }
    x += trailingGap.x * unit;
    sepU = x; x += metrics.separator;
    cells[HitTray].u0 = x; cells[HitTray].u1 = x + metrics.trayButton;
    stackButtons = metrics.stack;
    x = ArrangeGroup(0, metrics, cells[HitTray].u1, true);
    cells[HitClock].u0 = x; cells[HitClock].u1 = x + metrics.clock;
    cells[HitSensor].u0 = cells[HitClock].u1; cells[HitSensor].u1 = cells[HitSensor].u0 + metrics.sensor;
    x = ArrangeGroup(1, metrics, cells[HitSensor].u1, true);
    barU0 = start; barU1 = x + pad;
}

POINT Dock::ScreenPointUV(float u, float v) const {
    auto p = W(u, v);
    return { winX + (LONG)p.x, winY + (LONG)p.y };
}

RECT Dock::BarScreenRect() const {
    auto r = RectUV(barU0, barU1, margin - hideOff, margin + barThick - hideOff);
    return { winX + (LONG)r.left, winY + (LONG)r.top, winX + (LONG)r.right, winY + (LONG)r.bottom };
}

void Dock::ComputePanelRect() {
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? panelH : panelW, depth = vert ? panelW : panelH;
    float u = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f;
    u = clampf(u, across / 2 + 8 * scale, len - across / 2 - 8 * scale);
    float v0 = margin + barThick + 12 * scale - hideOff - (1 - panelAlpha.x) * 10 * scale;
    panelRect = RectUV(u - across / 2, u + across / 2, v0, v0 + depth);
}

bool Dock::CursorInPanel() const {
    if (panelAlpha.x < 0.05f && panelAlpha.target < 0.5f) return false;
    auto p = W(cursorU, cursorV);
    return p.x >= panelRect.left - 4 && p.x <= panelRect.right + 4 && p.y >= panelRect.top - 4 && p.y <= panelRect.bottom + 4;
}

D2D1_SIZE_F Dock::TextSize(IDWriteTextFormat* f, const std::wstring& s) {
    if (!f || s.empty()) return {};
    static std::unordered_map<std::wstring, D2D1_SIZE_F> cache;
    std::wstring key = std::to_wstring((uintptr_t)f) + L"|" + s;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    ComPtr<IDWriteTextLayout> tl;
    if (FAILED(g_gfx.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 200, &tl))) return {};
    DWRITE_TEXT_METRICS tm{};
    if (FAILED(tl->GetMetrics(&tm))) return {};
    if (cache.size() > 512) cache.clear();
    D2D1_SIZE_F size = { tm.widthIncludingTrailingWhitespace, tm.height };
    cache[key] = size;
    return size;
}

float Dock::TextWidth(IDWriteTextFormat* f, const std::wstring& s) {
    return TextSize(f, s).width;
}

D2D1_RECT_F Dock::ButtonRect(HitKind kind, bool hit) const {
    if (!FindSmallButton(kind)) return {};
    const Cell& c = cells[kind];
    if (c.u1 <= c.u0) return {};
    float u1 = c.u1;
    if (hit && u1 >= cells[HitSettings].u1 - 0.5f) u1 = std::max(u1, barU1);   // the last cell also takes clicks on the end padding
    auto r = RectUV(c.u0, u1, margin - hideOff, margin + barThick - hideOff);
    if (c.half >= 0) {
        // Split in screen coordinates: the first button is left (side docks) or on top
        // (top and bottom docks) on every edge.
        if (IsVertical(g_settings.edge)) { float mid = (r.left + r.right) * 0.5f; (c.half ? r.left : r.right) = mid; }
        else { float mid = (r.top + r.bottom) * 0.5f; (c.half ? r.top : r.bottom) = mid; }
    }
    return r;
}

Dock::Hit Dock::HitTestUV(float u, float v) {
    Hit h;
    if (panelAlpha.x > 0.3f) {
        auto p = W(u, v);
        if (p.x >= panelRect.left && p.x <= panelRect.right && p.y >= panelRect.top && p.y <= panelRect.bottom) { h.kind = HitPanel; return h; }
    }
    float vb0 = margin - hideOff, vb1 = margin + barThick - hideOff;
    if (u < barU0 - 2 || u > barU1 + 2) return h;
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        if (s.ghost || s.weight < 0.3f) continue;
        float top = std::max(vb1, margin + pad + s.size - hideOff);
        if (u >= s.u0 - gap * 0.5f && u <= s.u0 + s.size * s.weight + gap * 0.5f && v >= vb0 - 4 && v <= top) {
            h.kind = HitSlot; h.slot = (int)i; return h;
        }
    }
    if (v < vb0 - 4 || v > vb1) return h;
    const auto point = W(u, v);
    for (const SmallButton* b = kSmallButtons; b->kind != HitNone; ++b) {
        if (!Shown(*b)) continue;
        auto r = ButtonRect(b->kind, true);
        if (point.x >= r.left && point.x < r.right && point.y >= r.top && point.y < r.bottom) {
            h.kind = b->kind; return h;
        }
    }
    if (u >= cells[HitTray].u0 && u < cells[HitTray].u1) h.kind = HitTray;
    else if (u >= cells[HitClock].u0 && u < cells[HitClock].u1) h.kind = HitClock;
    else if (u >= cells[HitSensor].u0 && u < cells[HitSensor].u1) h.kind = HitSensor;
    else h.kind = HitBar;
    return h;
}

