#include <wpe/wpe-platform.h>

#include <cstdio>
#include <string_view>

int main()
{
    GError* error = nullptr;
    WPEDisplay* display = wpe_display_get_default();
    if (!display) {
        std::fputs("WPE Platform did not provide a default display\n", stderr);
        return 1;
    }

    if (!wpe_display_connect(display, &error)
        && (!error || std::string_view(error->message).find("already connected") == std::string_view::npos)) {
        std::fprintf(stderr, "Could not connect WPE display: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        return 1;
    }
    g_clear_error(&error);

    WPEView* view = wpe_view_new(display);
    if (!view) {
        std::fputs("WPE Platform could not create a view\n", stderr);
        return 1;
    }

    std::printf("WPE Platform display connected; created %s (%dx%d)\n",
        G_OBJECT_TYPE_NAME(view), wpe_view_get_width(view), wpe_view_get_height(view));
    g_object_unref(view);
    return 0;
}
