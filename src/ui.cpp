// Kleines Immediate-Mode-UI: Flaechen, Knoepfe, Texte. Wird von HUD, Menue und Editor benutzt.
#include "game.h"

Ui ui;

void Ui::begin(float w, float h, float mouseX, float mouseY, bool isDown, bool wasClicked) {
    geo.clear(); text.clear();
    W = w; H = h; mx = mouseX; my = mouseY; down = isDown; clicked = wasClicked; captured = false;
}

void Ui::panel(float x, float y, float w, float h, Color c, float r) {
    geo.rrect(x, y, w, h, r, c);
    if (over(x, y, w, h)) captured = true;
}

bool Ui::button(float x, float y, float w, float h, const std::string& s, bool active, float px) {
    bool hot = over(x, y, w, h);
    if (hot) captured = true;
    Color c = active ? Color{255, 184, 48, 255} : (hot ? col::btnHot : col::btn);
    geo.rrect(x, y, w, h, 7, c);
    labelC(x + w * 0.5f, y + h * 0.5f + px * 0.34f, px, active ? Color{20, 16, 6, 255} : col::white, s);
    bool hit = hot && clicked;
    if (hit) clicked = false; // ein Klick loest nur einen Knopf aus
    return hit;
}

void Ui::label(float x, float y, float px, Color c, const std::string& s) { font.draw(text, x, y, px, c, s); }
void Ui::labelC(float cx, float y, float px, Color c, const std::string& s) { font.draw(text, cx - font.width(s, px) * 0.5f, y, px, c, s); }
void Ui::labelR(float rx, float y, float px, Color c, const std::string& s) { font.draw(text, rx - font.width(s, px), y, px, c, s); }

void Ui::flush(const Mat4& mvp) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gfxBias(0);
    gfxModel(identity());
    gfxMVP(mvp);
    gfxMode(0, 0);
    geo.draw();
    gfxMode(5);
    gfxBindTexture(font.tex);
    text.draw();
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

std::vector<std::string> wrapText(const std::string& s, float px, float maxW) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&] {
        if (word.empty()) return;
        std::string t = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.width(t, px) > maxW) { lines.push_back(line); line = word; }
        else line = t;
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ') flushWord();
        else if (ch == '\n') { flushWord(); lines.push_back(line); line.clear(); }
        else word += ch;
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}
