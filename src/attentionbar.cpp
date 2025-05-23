/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 2008-2025 Vaclav Slavik
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

#include "attentionbar.h"

#include "colorscheme.h"
#include "custom_buttons.h"
#include "customcontrols.h"
#include "hidpi.h"
#include "utility.h"

#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/config.h>
#include <wx/dcclient.h>
#include <wx/wupdlock.h>

#ifdef __WXOSX__
    #define SMALL_BORDER   PX(7)
    #define BUTTONS_SPACE PX(10)
#else
    #define SMALL_BORDER   PX(3)
    #define BUTTONS_SPACE  PX(5)
#endif

BEGIN_EVENT_TABLE(AttentionBar, wxPanel)
    EVT_BUTTON(wxID_CLOSE, AttentionBar::OnClose)
    EVT_BUTTON(wxID_ANY, AttentionBar::OnAction)
END_EVENT_TABLE()

AttentionBar::AttentionBar(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
              wxTAB_TRAVERSAL | wxBORDER_NONE)
{
#ifdef __WXOSX__
    NSView *view = GetHandle();
    view.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
#endif

#ifdef __WXMSW__
    m_icon = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
#endif
    m_label = new AutoWrappingText(this, wxID_ANY, "");
    m_explanation = new ExplanationLabel(this, "");
    m_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_checkbox = new wxCheckBox(this, wxID_ANY, "");

    auto btnClose = new wxBitmapButton
                    (
                        this, wxID_CLOSE,
                        wxArtProvider::GetBitmap("window-close", wxART_MENU),
                        wxDefaultPosition, wxDefaultSize,
                        wxNO_BORDER
                    );
    btnClose->SetToolTip(_("Hide this notification message"));

#if defined(__WXOSX__) || defined(__WXMSW__)
    wxFont boldFont = m_label->GetFont();
    boldFont.SetWeight(MSW_OR_OTHER(wxFONTWEIGHT_BOLD, wxFONTWEIGHT_SEMIBOLD));
    m_label->SetFont(boldFont);
#endif

    Bind(wxEVT_PAINT, &AttentionBar::OnPaint, this);

    wxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddSpacer(PXDefaultBorder);
#ifdef __WXMSW__
    sizer->Add(m_icon, wxSizerFlags().Center().Border(wxALL, SMALL_BORDER));
#endif

    auto labelSizer = new wxBoxSizer(wxVERTICAL);
    labelSizer->Add(m_label, wxSizerFlags().Expand());
    labelSizer->AddSpacer(PX(1));
    labelSizer->Add(m_explanation, wxSizerFlags().Expand().Border(wxRIGHT, PX(4)));
    sizer->Add(labelSizer, wxSizerFlags(1).Center().PXDoubleBorder(wxALL));
    sizer->AddSpacer(PX(20));
    auto allButtonsSizer = new wxBoxSizer(wxHORIZONTAL);
    auto buttonsAndCheckboxSizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(buttonsAndCheckboxSizer, wxSizerFlags().Center().Border(wxTOP, PX(1)));
    buttonsAndCheckboxSizer->Add(allButtonsSizer, wxSizerFlags().Expand());
    buttonsAndCheckboxSizer->Add(m_checkbox, wxSizerFlags().Left().Border(wxTOP, MACOS_OR_OTHER(PX(2), PX(4))));
    allButtonsSizer->Add(m_buttons);
    allButtonsSizer->AddStretchSpacer();
    allButtonsSizer->AddSpacer(SMALL_BORDER);
    allButtonsSizer->Add(btnClose, wxSizerFlags().Center().Border(wxTOP, PX(1)));
    allButtonsSizer->AddSpacer(SMALL_BORDER);
#ifdef __WXMSW__
    sizer->AddSpacer(PX(4));
#endif

    SetSizer(sizer);

    // the bar should be initially hidden
    Show(false);

    ColorScheme::SetupWindowColors(this, [=]
    {
        UpdateBgColor();

    #ifndef __WXOSX__
        // The background is light even in dark mode, so we can't use system label colors in it:
        if (ColorScheme::GetAppMode() == ColorScheme::Light)
        {
            m_label->SetForegroundColour(ColorScheme::Get(Color::Label));
            m_explanation->SetForegroundColour(ColorScheme::Get(Color::SecondaryLabel));
        }
        else
        {
            m_label->SetForegroundColour(*wxBLACK);
            m_explanation->SetForegroundColour(*wxBLACK);
        }
    #endif

    #ifdef __WXMSW__
        btnClose->SetBackgroundColour(GetBackgroundColour());
    #endif
    });
}


void AttentionBar::OnPaint(wxPaintEvent&)
{
    wxPaintDC dc(this);

    auto bg = GetBackgroundColour().ChangeLightness(80);
    dc.SetBrush(bg);
    dc.SetPen(bg);

    wxRect rect(GetSize());
    dc.DrawRectangle(0, rect.height - MACOS_OR_OTHER(0, PX(1)), rect.width, PX(1));
}


void AttentionBar::UpdateBgColor()
{
    wxColour bg;

    switch (m_currentKind)
    {
        case AttentionMessage::Warning:
            bg = ColorScheme::Get(Color::AttentionWarningBackground);
            break;
        case AttentionMessage::Question:
            bg = ColorScheme::Get(Color::AttentionQuestionBackground);
            break;
        case AttentionMessage::Error:
            bg = ColorScheme::Get(Color::AttentionErrorBackground);
            break;
    }

    SetBackgroundColour(bg);

#ifdef __WXMSW__
    for (auto w : GetChildren())
        w->SetBackgroundColour(bg);
#endif
}


void AttentionBar::ShowMessage(const AttentionMessage& msg)
{
    if ( msg.IsBlacklisted() )
        return;

    m_currentKind = msg.m_kind;
    UpdateBgColor();

    wxString iconName;
    switch ( msg.m_kind )
    {
        case AttentionMessage::Warning:
            iconName = wxART_WARNING;
            break;
        case AttentionMessage::Question:
            iconName = wxART_QUESTION;
            break;
        case AttentionMessage::Error:
            iconName = wxART_ERROR;
            break;
    }
#ifdef __WXMSW__
    m_icon->SetBitmap(wxArtProvider::GetBitmap(iconName, wxART_MENU, wxSize(PX(16), PX(16))));
#endif

    m_label->SetAndWrapLabel(msg.m_text);
    m_explanation->SetAndWrapLabel(msg.m_explanation);
    m_explanation->GetContainingSizer()->Show(m_explanation, !msg.m_explanation.empty());
    m_checkbox->SetLabel(msg.m_checkbox);
    m_checkbox->GetContainingSizer()->Show(m_checkbox, !msg.m_checkbox.empty());

    m_buttons->Clear(true/*delete_windows*/);
    m_actions.clear();

    for ( AttentionMessage::Actions::const_iterator i = msg.m_actions.begin();
          i != msg.m_actions.end(); ++i )
    {
        auto b = new TranslucentButton(this, wxID_ANY, i->first);
        m_buttons->Add(b, wxSizerFlags().Center().Border(wxRIGHT, BUTTONS_SPACE));
        m_actions[b] = i->second;
    }

    // we need to size the control correctly _and_ lay out the controls if this
    // is the first time it's being shown, otherwise we can get garbled look:
    wxWindowUpdateLocker lock(this);
    SetSize(GetParent()->GetClientSize().x, 1);
    Layout();

    Refresh();
    Show();
    GetParent()->Layout();
}

void AttentionBar::HideMessage()
{
    Hide();
    GetParent()->Layout();
}

void AttentionBar::OnClose(wxCommandEvent& WXUNUSED(event))
{
    HideMessage();
}

void AttentionBar::OnAction(wxCommandEvent& event)
{
    ActionsMap::const_iterator i = m_actions.find(event.GetEventObject());
    if ( i == m_actions.end() )
    {
        event.Skip();
        return;
    }

    // first perform the action...
    AttentionMessage::ActionInfo info;
    info.checkbox = m_checkbox->IsShown() && m_checkbox->IsChecked();
    i->second(info);

    // ...then hide the message
    HideMessage();
}


/* static */
void AttentionMessage::AddToBlacklist(const wxString& id)
{
    wxConfig::Get()->Write
        (
            wxString::Format("/messages/dont_show/%s", id.c_str()),
            (long)true
        );
}

/* static */
bool AttentionMessage::IsBlacklisted(const wxString& id)
{
    return wxConfig::Get()->ReadBool
        (
            wxString::Format("/messages/dont_show/%s", id.c_str()),
            false
        );
}

void AttentionMessage::AddDontShowAgain()
{
    auto id = m_id;
    AddAction(
        MSW_OR_OTHER(_(L"Don’t show again"), _(L"Don’t Show Again")), [id]{
        AddToBlacklist(id);
    });
}
