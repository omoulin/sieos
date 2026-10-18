// uicxx: the UI toolkit (sieos/ui.h) from C++, as a libposix program: a
// window with a label, a field and a button that counts its clicks; its
// right-click menu has Quit (Ctrl+Q). Built by `make uicxx` (and by
// browser-test) to keep the header and libui-posix.a usable from C++.
//
// Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
#include <sieos/ui.h>
#include <cstdio>
#include <string>

namespace {
struct Counter {
    ui_widget *label = nullptr;
    int clicks = 0;
    static void clicked(ui_widget *, void *self)
    {
        auto *c = static_cast<Counter *>(self);
        std::string t = "Clicked " + std::to_string(++c->clicks) + " times";
        ui_set_text(c->label, t.c_str());
    }
};
void quit(ui_widget *w, void *) { ui_quit(ui_app_of(w), 0); }
}

int main()
{
    ui_app *app = ui_app_new("C++ and the toolkit");
    if (!app) { std::puts("uicxx: no desktop"); return 1; }
    Counter counter;
    ui_widget *col = ui_box(ui_root(app), UI_COL, 10, 16);
    ui_widget *menu = ui_menu(app, nullptr, "Menu");
    ui_item(menu, "Quit", UI_CTRL('q'), quit, nullptr);
    counter.label = ui_label(col, "Not clicked yet");
    ui_field(col, "Type here", nullptr, nullptr);
    ui_button(col, "Click me", Counter::clicked, &counter);
    ui_set_menu(col, menu);
    return ui_run(app);
}
