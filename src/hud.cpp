#include "hud.hpp"

#include <algorithm>
#include <cctype>

namespace cb2hud {

using namespace cb2ui;

void Hud::notice(const std::string& text, Rgb colour, double now) {
    std::string up = text;
    for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));   // the font has capitals only
    notices.push_back({up, colour, now + 5.0});
    if (notices.size() > 3) notices.erase(notices.begin());
}

void Hud::draw(Canvas& canvas, const std::vector<Row>& rows, bool panel, double now) {
    notices.erase(std::remove_if(notices.begin(), notices.end(), [&](const Notice& n) { return n.until < now; }), notices.end());
    float y = 66;
    for (const Notice& n : notices) {
        const float fade = float(std::min(1.0, (n.until - now) / 0.6));   // fades out over the last 0.6 s
        const float w = canvas.width(n.text, 16) + 20;
        canvas.rect(10, y, w, 24, rgb(0, 6, 12), 0.62f * fade);
        canvas.rect(10, y, 4, 24, n.colour, fade);
        canvas.text(n.text, 22, y + 4, 16, n.colour);
        y += 28;
    }
    if (panel && !rows.empty()) {
        const float w = 196, rowH = 38, x = kW - w - 8, top = 64;
        const float h = 30 + rowH * float(rows.size());
        canvas.rect(x, top, w, h, rgb(0, 6, 12), 0.6f);
        canvas.rect(x, top, w, 3, rgb(150, 92, 38), 0.95f);
        canvas.text("PLAYERS  " + std::to_string(rows.size()), x + 10, top + 7, 15, kGold1, kGold2);
        float ry = top + 28;
        for (const Row& r : rows) {
            const Rgb c = rgb(uint8_t(r.rgb >> 16), uint8_t(r.rgb >> 8), uint8_t(r.rgb));
            canvas.rect(x + 8, ry + 5, 5, rowH - 12, c);
            std::string name = r.name.size() > 12 ? r.name.substr(0, 12) : r.name;
            for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            canvas.text(name + (r.self ? "  (YOU)" : ""), x + 20, ry + 2, 15, kWhite1, kWhite2);
            canvas.text(r.where, x + 20, ry + 19, 12, r.inLevel ? kTeal : kDim);
            if (!r.time.empty()) canvas.text(r.time, x + w - 10 - canvas.width(r.time, 13), ry + 19, 13, r.inLevel ? kGreen : kDim);
            ry += rowH;
        }
    }
    canvas.flush();
}

}  // namespace cb2hud
