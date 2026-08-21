// ztpp — src/launcher_gtk.cpp: НАТИВНОЕ окно выбора ROM на Linux (GTK3), по образцу стартап-окна
// eduke32 (source/duke3d/src/startgtk.game.c). Реальные виджеты ОС: GtkWindow + GtkTreeView
// (список ROM, столбцы File/Build/Size/Status; неподдерж. билды серые) + кнопки Play/Quit/Browse
// (GtkFileChooserDialog — нативный файловый диалог) + drag&drop (uri-list) + GtkMessageDialog.
//
// ⚠⚠ НЕ ПРОВЕРЕНО СБОРКОЙ: разработка ztpp идёт на macOS, здесь нет GTK-заголовков. Код написан
// best-effort по GTK3 API — при первой сборке на Linux возможны правки. Data-слой
// (scanRoms/probeRom/makeState/saveLastRom) — общий с macOS/Windows (launcher_data.hpp).
//
// Модель eduke32: gtk_main()/gtk_main_quit() в обработчиках Play/Quit с сентинелом result, окно ДО
// инициализации SDL-окна игры. CMake добавляет файл только если найден gtk+-3.0 (иначе Linux
// остаётся на SDL-фолбэке); тогда же ставит ZTPP_GTK_LAUNCHER, и диспетчер run() выбирает runNative.

#ifdef ZTPP_GTK_LAUNCHER

#include "launcher_data.hpp"   // launcher::Entry / scanRoms / makeState / probeRom / buildLabel / saveLastRom
#include "version.hpp"
#include <gtk/gtk.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum { LR_RUNNING = -1, LR_QUIT = 0, LR_PLAY = 1 };
// Колонки GtkListStore: текст File/Build/Size/Status + цвет (грей для неподдерж.) + флаг применять-цвет.
enum { COL_FILE, COL_BUILD, COL_SIZE, COL_STATUS, COL_FG, COL_FGSET, N_COLS };

struct GtkCtx {
    std::vector<launcher::Entry>* entries = nullptr;
    GtkWidget*    window = nullptr;
    GtkWidget*    tree   = nullptr;
    GtkListStore* store  = nullptr;
    GtkWidget*    play    = nullptr;
    GtkWidget*    detail1 = nullptr;
    GtkWidget*    detail2 = nullptr;
    int result = LR_RUNNING;
    std::string selectedPath;
};

void populate(GtkCtx* c) {
    gtk_list_store_clear(c->store);
    for (const launcher::Entry& e : *c->entries) {
        char sizebuf[32]; std::snprintf(sizebuf, sizeof(sizebuf), "%.1f MB", (double)e.size / (1024.0 * 1024.0));
        const char* fg = e.supported ? nullptr : "#888888";
        switch (launcher::buildTextTone(e.build)) {
            case launcher::TextTone::Green:  fg = "#33B84D"; break;
            case launcher::TextTone::Yellow: fg = "#D9A91C"; break;
            case launcher::TextTone::Red:    fg = "#E64038"; break;
            default: break;
        }
        GtkTreeIter it;
        gtk_list_store_append(c->store, &it);
        gtk_list_store_set(c->store, &it,
            COL_FILE,   e.file.c_str(),
            COL_BUILD,  launcher::buildLabel(e.build),
            COL_SIZE,   sizebuf,
            COL_STATUS, launcher::buildStatus(e.build),
            COL_FG,     fg,
            COL_FGSET,  fg ? TRUE : FALSE,
            -1);
    }
}

int selectedIndex(GtkCtx* c) {
    GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->tree));
    GtkTreeModel* model; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return -1;
    GtkTreePath* path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return idx;
}

void selectRow(GtkCtx* c, int i) {
    if (i < 0 || i >= (int)c->entries->size()) return;
    GtkTreeIter it;
    if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(c->store), &it, nullptr, i)) {
        GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->tree));
        gtk_tree_selection_select_iter(sel, &it);
        GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(c->store), &it);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->tree), path, nullptr, FALSE, 0, 0);
        gtk_tree_path_free(path);
    }
}

void updateDetails(GtkCtx* c) {
    int i = selectedIndex(c);
    if (i < 0 || i >= (int)c->entries->size()) {
        gtk_label_set_text(GTK_LABEL(c->detail1), c->entries->empty()
            ? "No Zero Tolerance ROMs found — use Browse… or drag & drop a .gen/.bin/.md file" : "");
        gtk_label_set_text(GTK_LABEL(c->detail2), "");
        gtk_widget_set_sensitive(c->play, FALSE);
        return;
    }
    const launcher::Entry& e = (*c->entries)[(size_t)i];
    std::string d1 = "Title: " + e.title + "    Serial: " + e.serial;
    std::string d2 = "Path: " + e.path;
    gtk_label_set_text(GTK_LABEL(c->detail1), d1.c_str());
    gtk_label_set_text(GTK_LABEL(c->detail2), d2.c_str());
    gtk_widget_set_sensitive(c->play, TRUE);
}

void doPlay(GtkCtx* c) {
    int i = selectedIndex(c);
    if (i < 0 || i >= (int)c->entries->size()) return;
    const launcher::Entry& e = (*c->entries)[(size_t)i];
    if (!e.supported) {
        GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(c->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "This build is not supported yet");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
            "Only Zero Tolerance (release) and ZT Underground (partial support) can be launched right now.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    c->selectedPath = e.path;
    c->result = LR_PLAY;
    gtk_main_quit();
}

void addRom(GtkCtx* c, const char* path) {
    launcher::Entry e;
    if (!path || !launcher::probeRom(path, e, /*keepUnknown=*/true)) {
        GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(c->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Not a Mega Drive ROM");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
            "The file does not look like a Sega Mega Drive / Genesis ROM.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    for (int i = 0; i < (int)c->entries->size(); ++i)
        if ((*c->entries)[(size_t)i].path == e.path) { selectRow(c, i); return; }
    c->entries->insert(c->entries->begin(), e);
    populate(c);
    selectRow(c, 0);
}

// ─── Сигналы ─────────────────────────────────────────────────────────────────────
void onPlay(GtkButton*, gpointer u)  { doPlay((GtkCtx*)u); }
void onQuit(GtkButton*, gpointer u)  { GtkCtx* c = (GtkCtx*)u; c->result = LR_QUIT; gtk_main_quit(); }
void onRowActivated(GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, gpointer u) { doPlay((GtkCtx*)u); }
void onSelChanged(GtkTreeSelection*, gpointer u) { updateDetails((GtkCtx*)u); }
gboolean onDelete(GtkWidget*, GdkEvent*, gpointer u) { GtkCtx* c = (GtkCtx*)u; c->result = LR_QUIT; gtk_main_quit(); return TRUE; }

void onBrowse(GtkButton*, gpointer u) {
    GtkCtx* c = (GtkCtx*)u;
    GtkWidget* dlg = gtk_file_chooser_dialog_new("Choose a Mega Drive ROM", GTK_WINDOW(c->window),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "Mega Drive ROM (*.gen, *.bin, *.md)");
    gtk_file_filter_add_pattern(f, "*.gen");
    gtk_file_filter_add_pattern(f, "*.bin");
    gtk_file_filter_add_pattern(f, "*.md");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn) { addRom(c, fn); g_free(fn); }
    }
    gtk_widget_destroy(dlg);
}

void onDrop(GtkWidget*, GdkDragContext* dctx, gint, gint, GtkSelectionData* data,
            guint, guint time, gpointer u) {
    GtkCtx* c = (GtkCtx*)u;
    gchar** uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        char* path = g_filename_from_uri(uris[0], nullptr, nullptr);
        if (path) { addRom(c, path); g_free(path); }
    }
    if (uris) g_strfreev(uris);
    gtk_drag_finish(dctx, TRUE, FALSE, time);
}

void addTextColumn(GtkCtx* c, const char* title, int col, gfloat xalign) {
    GtkCellRenderer* r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, nullptr);
    GtkTreeViewColumn* tc = gtk_tree_view_column_new_with_attributes(title, r,
        "text", col, "foreground", COL_FG, "foreground-set", COL_FGSET, nullptr);
    gtk_tree_view_column_set_resizable(tc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c->tree), tc);
}

} // anonymous namespace

namespace launcher {

std::string runNative(const std::string& preselect) {
    State st = makeState(preselect);   // общий скан + предвыбор (печатает список найденных ROM)

    if (!gtk_init_check(nullptr, nullptr)) {
        std::fprintf(stderr, "launcher: GTK init failed, using autodetected ROM\n");
        return preselect;              // без дисплея — как SDL-фолбэк при сбое init
    }

    GtkCtx c;
    c.entries = &st.entries;

    c.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    { char gt[64]; std::snprintf(gt, sizeof gt, "ZTPP v%s — Select a ROM", ztppVersion());
      gtk_window_set_title(GTK_WINDOW(c.window), gt); }
    gtk_window_set_default_size(GTK_WINDOW(c.window), 640, 440);
    gtk_window_set_position(GTK_WINDOW(c.window), GTK_WIN_POS_CENTER);
    g_signal_connect(c.window, "delete-event", G_CALLBACK(onDelete), &c);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(c.window), vbox);

    GtkWidget* header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(header),
        "<b>Zero Tolerance — C++ Port</b>\n<span size='small'>Select a ROM to launch  ·  drag &amp; drop or Browse to add one</span>");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0f);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    // Список
    c.store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    c.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c.store));
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(c.tree), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    addTextColumn(&c, "File",   COL_FILE,   0.0f);
    addTextColumn(&c, "Build",  COL_BUILD,  0.0f);
    addTextColumn(&c, "Size",   COL_SIZE,   1.0f);
    addTextColumn(&c, "Status", COL_STATUS, 0.0f);
    g_signal_connect(c.tree, "row-activated", G_CALLBACK(onRowActivated), &c);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(c.tree)), "changed", G_CALLBACK(onSelChanged), &c);

    GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), c.tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    // Детали
    c.detail1 = gtk_label_new("");
    c.detail2 = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(c.detail1), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(c.detail2), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(c.detail2), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(vbox), c.detail1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), c.detail2, FALSE, FALSE, 0);

    // Кнопки: Browse слева, Quit+Play справа
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* browse = gtk_button_new_with_label("Browse…");
    GtkWidget* quit   = gtk_button_new_with_label("Quit");
    c.play            = gtk_button_new_with_label("Play");
    g_signal_connect(browse,  "clicked", G_CALLBACK(onBrowse), &c);
    g_signal_connect(quit,    "clicked", G_CALLBACK(onQuit),   &c);
    g_signal_connect(c.play,  "clicked", G_CALLBACK(onPlay),   &c);
    gtk_box_pack_start(GTK_BOX(hbox), browse, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), c.play, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), quit,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    // Play = кнопка по умолчанию (Enter)
    gtk_widget_set_can_default(c.play, TRUE);
    gtk_widget_grab_default(c.play);

    // Drag&drop файлов на окно
    gtk_drag_dest_set(c.window, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(c.window);
    g_signal_connect(c.window, "drag-data-received", G_CALLBACK(onDrop), &c);

    populate(&c);
    if (st.sel >= 0 && st.sel < (int)st.entries.size()) selectRow(&c, st.sel);
    updateDetails(&c);

    gtk_widget_show_all(c.window);
    gtk_main();

    std::string out = (c.result == LR_PLAY) ? c.selectedPath : std::string();
    gtk_widget_destroy(c.window);
    while (gtk_events_pending()) gtk_main_iteration();   // дать окну закрыться

    if (!out.empty()) { saveLastRom(out); std::printf("launcher: selected %s\n", out.c_str()); }
    else                std::printf("launcher: no ROM selected, exit\n");
    return out;
}

} // namespace launcher

#endif // ZTPP_GTK_LAUNCHER
