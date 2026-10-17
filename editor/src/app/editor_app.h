#pragma once
// The editor application: owns the shared EditorContext and every panel, and
// draws one frame of the whole UI: menu bar, toolbar, dockspace with the
// panels (default layout via DockBuilder), status bar and modal dialogs.

#include "app/editor_context.h"
#include "app/panel.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace osef::editor {

class EditorApp {
public:
    EditorApp();
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    EditorContext& context() { return ctx_; }

    // Opens an OS project (directory or .cpp) and replaces the current one.
    // Logs failures (and returns them in *error).
    bool open_project(const std::filesystem::path& location, const std::filesystem::path& engine_root,
                      std::string* error = nullptr);

    // Call after ImGui::CreateContext() and before the first NewFrame():
    // persists panel visibility in imgui.ini next to the dock layout.
    void install_settings_handler();

    // Draws the whole UI. Call between ImGui::NewFrame() and ImGui::Render().
    void frame();

    // Window close button / Cmd+Q / signals: asks about unsaved changes first.
    // `force` quits without asking (e.g. a second SIGTERM).
    void request_quit(bool force = false);
    bool should_quit() const { return quit_confirmed_; }

    // "OSEF Editor — <OS name> — <main.cpp path>" + " ●" while anything is unsaved.
    std::string window_title() const;

    // Screenshot mode never opens blocking dialogs on its own.
    void set_screenshot_mode(bool on) { screenshot_mode_ = on; }

    // Rebuilds the default dock layout on the next frame.
    void reset_layout() { layout_reset_pending_ = true; }

    Panel* find_panel(std::string_view title);
    const std::vector<std::unique_ptr<Panel>>& panels() const { return panels_; }

private:
    void handle_shortcuts();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_status_bar();
    void draw_dockspace();
    void draw_panels();
    void draw_dialogs();
    void build_default_layout(unsigned int dockspace_id, float width, float height);

    void save_active();
    void save_all();
    void start_target(const std::string& id);
    void edit_command(int command);
    bool any_modified() const;

    EditorContext ctx_;
    std::vector<std::unique_ptr<Panel>> panels_;

    bool screenshot_mode_ = false;
    bool layout_reset_pending_ = false;
    bool layout_built_ = false;
    int focus_console_tab_frames_ = 0;  // after building the layout: show the Console tab
    int find_inject_ = 0;               // Edit > Find: forwards Cmd/Ctrl+F to the code panel

    bool quit_confirmed_ = false;
    bool open_quit_prompt_ = false;
    bool open_project_dialog_ = false;
    bool open_new_os_ = false;
    bool new_os_folder_edited_ = false;
    std::string new_os_name_ = "MyOS";
    std::string new_os_folder_;
    std::string new_os_error_;
    int new_os_memory_ = 1;                 // index into {64, 128, 256, 512} MiB
    int new_os_look_ = 0;
    bool new_os_uefi_ = true, new_os_screen_ = true, new_os_keyboard_ = true, new_os_serial_ = true,
         new_os_clock_ = true, new_os_pci_ = false, new_os_usb_ = false, new_os_samples_ = true;
    void draw_new_os_dialog();
    bool open_about_ = false;
    bool open_shortcuts_ = false;
    bool open_revert_ = false;
    char project_path_[1024] = {};
    std::string project_dialog_error_;
    std::string last_panel_error_;
};

} // namespace osef::editor
