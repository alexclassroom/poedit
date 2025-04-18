/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2014-2025 Vaclav Slavik
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

#ifndef Poedit_customcontrols_h
#define Poedit_customcontrols_h

#include "colorscheme.h"
#include "concurrency.h"
#include "language.h"

#include <wx/bmpbuttn.h>
#include <wx/dataview.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/hyperlink.h>
#include <wx/xrc/xmlres.h>

class WXDLLIMPEXP_ADV wxActivityIndicator;

#include <exception>
#include <functional>


/// Label marking a subsection of a dialog:
class HeadingLabel : public wxStaticText
{
public:
    HeadingLabel(wxWindow *parent, const wxString& label);
};

/// Label that auto-wraps itself to fit surrounding control's width.
class AutoWrappingText : public wxStaticText
{
public:
    AutoWrappingText(wxWindow *parent, wxWindowID winid, const wxString& label);

    void SetLanguage(Language lang);
    void SetAlignment(TextDirection dir);

    void SetAndWrapLabel(const wxString& label);

    bool InformFirstDirection(int direction, int size, int availableOtherDir) override;

    void SetLabel(const wxString& label) override;

protected:
    void OnSize(wxSizeEvent& e);
    bool RewrapForWidth(int width);

#ifdef __WXOSX__
    wxSize DoGetBestSize() const override;
#endif

    wxString m_text;
    int m_wrapWidth;
    Language m_language;
};

/// A helper class that implements better sizer behavior for a window that contains AutoWrappingText
template<typename Base>
class WindowWith2DSizingConstraints : public Base
{
public:
    using Base::Base;

    void Fit() override
    {
        IterateUntilConvergence([=]{ this->SetSize(this->GetBestSize()); });
    }

    bool Layout() override
    {
        IterateUntilConvergence([=]{ Base::Layout(); });
        return true;
    }

private:
    template<typename T>
    void IterateUntilConvergence(T&& action)
    {
        // iterate sizing because performing layout may invalidate some best
        // sizes (AutoWrappingText) and may need to be redone.
        wxSize best = this->GetBestSize();
        while ( true )
        {
            action();
            wxSize updatedBest = this->GetBestSize();
            if ( best == updatedBest )
                break;
            best = updatedBest;
        }
    }
};


/// Like AutoWrappingText, but allows selecting (macOS) or at least copying (Windows)
/// the text too.
class SelectableAutoWrappingText : public AutoWrappingText
{
public:
    SelectableAutoWrappingText(wxWindow *parent, wxWindowID winid, const wxString& label);
};


/** 
    Longer, often multiline, explanation label used to provide more information
    about the effects of some less obvious settings. Typeset using smaller font
    on macOS and grey appearance. Auto-wraps itself to fit surrounding control's
    width.
 */
class ExplanationLabel : public AutoWrappingText
{
public:
    ExplanationLabel(wxWindow *parent, const wxString& label);

#if defined(__WXOSX__)
    static const int CHECKBOX_INDENT = 21;
#elif defined(__WXMSW__)
    static const int CHECKBOX_INDENT = 17;
#elif defined(__WXGTK__)
    static const int CHECKBOX_INDENT = 25;
#endif

    static wxColour GetTextColor() { return ColorScheme::Get(Color::SecondaryLabel); }
};

/// Like ExplanationLabel, but nonwrapping
class SecondaryLabel : public wxStaticText
{
public:
    SecondaryLabel(wxWindow *parent, const wxString& label);

    static wxColour GetTextColor() { return ExplanationLabel::GetTextColor(); }
};


/// "Learn more" hyperlink for dialogs.
class LearnMoreLink : public wxHyperlinkCtrl
{
public:
    LearnMoreLink(wxWindow *parent, const wxString& url, wxString label = wxString(), wxWindowID winid = wxID_ANY);
};

class LearnMoreLinkXmlHandler : public wxXmlResourceHandler
{
public:
    LearnMoreLinkXmlHandler() {}
    wxObject *DoCreateResource() override;
    bool CanHandle(wxXmlNode *node) override;
};


/// Indicator of background activity, using spinners where appropriate.
class ActivityIndicator : public wxWindow
{
public:
    enum Flags
    {
        Centered = 1,
    };

    ActivityIndicator(wxWindow *parent, int flags = 0);

    /// Start indicating, with optional progress label.
    void Start(const wxString& msg = "");

    /// Stop the indicator.
    void Stop();

    /// Stop the indicator and report error in its place.
    void StopWithError(const wxString& msg);

    /// Is between Start() and Stop() calls?
    bool IsRunning() const { return m_running; }

    /// Convenience function for showing error message in the indicator
    std::function<void(dispatch::exception_ptr)> HandleError;

    bool HasTransparentBackground() override { return true; }

private:
    void UpdateLayoutAfterTextChange();

    bool m_running;
    wxActivityIndicator *m_spinner;
    wxStaticText *m_label, *m_error;
};


/// A bit nicer (macOS), color scheme aware, and easier to use image button
class ImageButton : public wxBitmapButton
{
public:
    ImageButton(wxWindow *parent, const wxString& bitmapName);

private:
    wxString m_bitmapName;
};


/// Color scheme aware static bitmap
class StaticBitmap : public wxStaticBitmap
{
public:
    StaticBitmap(wxWindow *parent, const wxString& bitmapName);

    void SetBitmapName(const wxString& bitmapName);

private:
    wxString m_bitmapName;
};


/// Avatar icon
class AvatarIcon : public wxWindow
{
public:
    AvatarIcon(wxWindow *parent, const wxSize& size);

    /// Set name to be used if image can't be loaded
    void SetUserName(const wxString& name);
    void LoadIcon(const wxFileName& f);

    bool HasTransparentBackground() override { return true; }

private:
    void InitForSize();
    void OnPaint(wxPaintEvent&);

private:
    wxRegion m_clipping;
    wxBitmap m_bitmap;
    wxString m_placeholder;
};


/// wxDataViewListCtrl with icon and two-line content
class IconAndSubtitleListCtrl : public wxDataViewListCtrl
{
public:
    IconAndSubtitleListCtrl(wxWindow *parent, const wxString& columnTitle, long style = wxBORDER_NONE);

    void AppendFormattedItem(const wxBitmap& icon, const wxString& title, const wxString& description);
    void UpdateFormattedItem(unsigned row, const wxString& title, const wxString& description);

protected:
    int GetDefaultRowHeight() const;
    wxString FormatItemText(const wxString& title, const wxString& description);

private:
#ifndef __WXGTK__
    void OnColorChange();
    wxString GetSecondaryFormatting();
    wxString m_secondaryFormatting[2];
#endif

    class MultilineTextRenderer;
};

#endif // Poedit_customcontrols_h
