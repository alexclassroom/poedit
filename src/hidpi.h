/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2015-2025 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef Poedit_hidpi_h
#define Poedit_hidpi_h

#include <wx/defs.h>
#include <wx/font.h>
#include <wx/image.h>

class WXDLLIMPEXP_FWD_BASE wxString;

#ifdef __WXMSW__
    #define NEEDS_MANUAL_HIDPI 1
#endif

#ifdef NEEDS_MANUAL_HIDPI

// Scaling factor against "normal" DPI (2.0 would be macOS's "Retina" scaling)
extern double g_pxScalingFactor;

/// Returns current scaling factor.
inline double HiDPIScalingFactor() { return g_pxScalingFactor; }

/// Is the current mode HiDPI?
inline bool IsHiDPI() { return g_pxScalingFactor > 1.0; }

/**
    Use this macro to wrap pixel dimensions to scale them accordingly to the
    current DPI setting.
 */
#define PX(x) (int(((x) * g_pxScalingFactor) + 0.5))

#define PXDefaultBorder wxSizerFlags::GetDefaultBorder()
#define PXBorder(dir) Border(dir, PXDefaultBorder)
#define PXDoubleBorder(dir) Border(dir, 2 * PXDefaultBorder)

/// Initializes HiDPI code, should be called early in OnInit.
void InitHiDPIHandling();

#else // ! NEEDS_MANUAL_HIDPI
#define PX(x) (x)
#define PXDefaultBorder wxSizerFlags::GetDefaultBorder()
#define PXBorder(dir) Border(dir)
#define PXDoubleBorder(dir) DoubleBorder(dir)
inline void InitHiDPIHandling() {}
inline double HiDPIScalingFactor() { return 1.0; }
#endif

#define PXBorderAll() PXBorder(wxALL)
#define PXDoubleBorderAll() PXDoubleBorder(wxALL)


/// Fine-tuned creation of smaller fonts
inline wxFont SmallerFont(const wxFont& font)
{
#ifdef __WXMSW__
    // wxFont::Smaller() is uses precise fractional font sizes, which looks 
    // ugly in low-DPI and small font sizes.
    if (!IsHiDPI())
    {
        wxFont f(font);
        f.SetPointSize(int(f.GetPointSize() / 1.2f + 0.5));
        return f;
    }
#endif
    return font.Smaller();
}


// helper for handling scaled images
// TODO: replace with use of wxBitmapBundle & remove #include <wx/image.h>
struct ScaledImage
{
    wxImage image;
    double scale = 1.0;

    bool IsOk() const { return image.IsOk(); }
};

/**
    Load image from given PNG file.

    Depending on the current scaling factor, the file loaded may be
    a @2x variant (e.g. "foo@2x.png" instead of "foo.png" for "foo"
    argument). In any case, the bitmap will be scaled appropriately.

    Note that @a name is given *without* the ".png" extension.
 */
extern ScaledImage LoadScaledBitmap(const wxString& name);

#endif // Poedit_hidpi_h
