#ifndef YUNTYPE_WIN_OVERLAY_H
#define YUNTYPE_WIN_OVERLAY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace yuntype::win {

struct OverlayModel {
    bool visible = false;
    std::string preedit;
    std::size_t preeditCursorByte = 0; // Highlight caret position in draft mode
    bool showDraftCaret = false;
    std::string statusSuffix;          // Compact indicator: "…", "·", "!"
    std::string auxText;               // Optional brief error/hint line
    std::vector<std::string> pageCandidates;
    int pageCursor = 0;                // Highlighted index within current page (0..pageSize-1)
    int currentPage = 0;
    int totalPages = 1;
    bool hasMoreAction = false;
};

class CandidateOverlay {
public:
    CandidateOverlay() = default;
    ~CandidateOverlay();

    CandidateOverlay(const CandidateOverlay &) = delete;
    CandidateOverlay &operator=(const CandidateOverlay &) = delete;

    bool create(HINSTANCE hInst, std::function<void(int)> onCandidateClick);
    void destroy();

    void update(const OverlayModel &model, const RECT &anchorRect);
    void hide();
    bool isVisible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void recomputeLayoutAndMove(const RECT &anchorRect);
    void paint(HDC hdc);
    int hitTestCandidate(POINT pt) const;

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    OverlayModel model_;
    RECT lastAnchor_{};
    int dpi_ = 96;
    int width_ = 220;
    int height_ = 48;
    int hoverIndex_ = -1;
    std::vector<RECT> itemRects_;
    std::function<void(int)> onCandidateClick_;
};

} // namespace yuntype::win

#endif
