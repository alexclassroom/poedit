/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 1999-2025 Vaclav Slavik
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

#include "catalog_po.h"

#include "configuration.h"
#include "errors.h"
#include "extractors/extractor.h"
#include "gexecute.h"
#include "str_helpers.h"
#include "utility.h"
#include "version.h"
#include "language.h"

#include <stdio.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>
#include <wx/log.h>
#include <wx/intl.h>
#include <wx/datetime.h>
#include <wx/config.h>
#include <wx/textfile.h>
#include <wx/scopeguard.h>
#include <wx/stdpaths.h>
#include <wx/strconv.h>
#include <wx/memtext.h>
#include <wx/filename.h>

#include <set>
#include <algorithm>

#ifdef __WXOSX__
#import <Foundation/Foundation.h>
#endif

// TODO: split into different file
#if wxUSE_GUI
    #include <wx/msgdlg.h>
#endif

// ----------------------------------------------------------------------
// Textfile processing utilities:
// ----------------------------------------------------------------------

namespace
{

// If input begins with pattern, fill output with end of input (without
// pattern; strips trailing spaces) and return true.  Return false otherwise
// and don't touch output. Is permissive about whitespace in the input:
// a space (' ') in pattern will match any number of any whitespace characters
// on that position in input.
bool ReadParam(const wxString& input, const wxString& pattern, wxString& output, bool preserveWhitespace = false)
{
    if (input.size() < pattern.size())
        return false;

    unsigned in_pos = 0;
    unsigned pat_pos = 0;
    while (pat_pos < pattern.size() && in_pos < input.size())
    {
        const wxChar pat = pattern[pat_pos++];

        if (pat == _T(' '))
        {
            if (!wxIsspace(input[in_pos++]))
                return false;

            if (!preserveWhitespace)
            {
                while (wxIsspace(input[in_pos]))
                {
                    in_pos++;
                    if (in_pos == input.size())
                        return false;
                }
            }
        }
        else
        {
            if (input[in_pos++] != pat)
                return false;
        }

    }

    if (pat_pos < pattern.size()) // pattern not fully matched
        return false;

    output = input.Mid(in_pos);
    if (!preserveWhitespace)
        output.Trim(true); // trailing whitespace
    return true;
}


// Checks if the file was loaded correctly, i.e. that non-empty lines
// ended up non-empty in memory, after doing charset conversion in
// wxTextFile. This detects for example files that claim they are in UTF-8
// while in fact they are not.
bool VerifyFileCharset(const wxTextFile& f, const wxString& filename,
                       const wxString& charset)
{
    wxTextFile f2;

    if (!f2.Open(filename, wxConvISO8859_1))
        return false;

    if (f.GetLineCount() != f2.GetLineCount())
    {
        int linesCount = (int)f2.GetLineCount() - (int)f.GetLineCount();
        wxLogError(wxPLURAL(L"%i line of file “%s” was not loaded correctly.",
                            L"%i lines of file “%s” were not loaded correctly.",
                            linesCount),
                   linesCount,
                   filename.c_str());
        return false;
    }

    bool ok = true;
    size_t cnt = f.GetLineCount();
    for (size_t i = 0; i < cnt; i++)
    {
        if (f[i].empty() && !f2[i].empty()) // wxMBConv conversion failed
        {
            wxLogError(
                _(L"Line %d of file “%s” is corrupted (not valid %s data)."),
                int(i), filename.c_str(), charset.c_str());
            ok = false;
        }
    }

    return ok;
}


wxTextFileType GetFileCRLFFormat(wxTextFile& po_file)
{
    wxLogNull null;
    auto crlf = po_file.GuessType();

    // Discard any unsupported setting. In particular, we ignore "Mac"
    // line endings, because the ancient OS 9 systems aren't used anymore,
    // OSX uses Unix ending *and* "Mac" endings break gettext tools. So if
    // we encounter a catalog with "Mac" line endings, we silently convert
    // it into Unix endings (i.e. the modern Mac).
    if (crlf == wxTextFileType_Mac)
        crlf = wxTextFileType_Unix;
    if (crlf != wxTextFileType_Dos && crlf != wxTextFileType_Unix)
        crlf = wxTextFileType_None;
    return crlf;
}

wxTextFileType GetDesiredCRLFFormat(wxTextFileType existingCRLF)
{
    if (existingCRLF != wxTextFileType_None && wxConfigBase::Get()->ReadBool("keep_crlf", true))
    {
        return existingCRLF;
    }
    else
    {
        wxString format = wxConfigBase::Get()->Read("crlf_format", "unix");
        if (format == "win")
            return wxTextFileType_Dos;
        else /* "unix" or obsolete settings */
            return wxTextFileType_Unix;
    }
}


unsigned GetCountFromPluralFormsHeader(const Catalog::HeaderData& header)
{
    if ( header.HasHeader("Plural-Forms") )
    {
        // e.g. "Plural-Forms: nplurals=3; plural=(n%10==1 && n%100!=11 ?
        //       0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\n"

        wxString form = header.GetHeader("Plural-Forms");
        form = form.BeforeFirst(_T(';'));
        if (form.BeforeFirst(_T('=')) == "nplurals")
        {
            wxString vals = form.AfterFirst('=');
            if (vals == "INTEGER") // POT default
                return 2;
            long val;
            if (vals.ToLong(&val))
                return (unsigned)val;
        }
    }

    // fallback value for plural forms count should be 2, as in English:
    return 2;
}

} // anonymous namespace


// ----------------------------------------------------------------------
// Parsers
// ----------------------------------------------------------------------

bool POCatalogParser::Parse()
{
    static const wxString prefix_flags(wxS("#, "));
    static const wxString prefix_autocomments(wxS("#. "));
    static const wxString prefix_autocomments2(wxS("#.")); // account for empty auto comments
    static const wxString prefix_references(wxS("#: "));
    static const wxString prefix_prev_msgid(wxS("#| "));
    static const wxString prefix_msgctxt(wxS("msgctxt \""));
    static const wxString prefix_msgid(wxS("msgid \""));
    static const wxString prefix_msgid_plural(wxS("msgid_plural \""));
    static const wxString prefix_msgstr(wxS("msgstr \""));
    static const wxString prefix_msgstr_plural(wxS("msgstr["));
    static const wxString prefix_deleted(wxS("#~"));
    static const wxString prefix_deleted_msgid(wxS("#~ msgid"));

    if (m_textFile->GetLineCount() == 0)
        return false;

    wxString line, dummy;
    wxString mflags, mstr, msgid_plural, mcomment;
    wxArrayString mrefs, mextractedcomments, mtranslations;
    wxArrayString msgid_old;
    bool has_plural = false;
    bool has_context = false;
    wxString msgctxt;
    unsigned mlinenum = 0;

    line = m_textFile->GetFirstLine();
    if (line.empty()) line = ReadTextLine();

    while (!line.empty())
    {
        // ignore empty special tags (except for extracted comments which we
        // DO want to preserve):
        while (line.length() == 2 && *line.begin() == '#' && (line[1] == ',' || line[1] == ':' || line[1] == '|'))
            line = ReadTextLine();

        // flags:
        // Can't we have more than one flag, now only the last is kept ...
        if (ReadParam(line, prefix_flags, dummy))
        {
            static wxString prefix_flags_partial(wxS(", "));
            mflags = prefix_flags_partial + dummy;
            line = ReadTextLine();
        }

        // auto comments:
        if (ReadParam(line, prefix_autocomments, dummy, /*preserveWhitespace=*/true) || ReadParam(line, prefix_autocomments2, dummy, /*preserveWhitespace=*/true))
        {
            mextractedcomments.Add(dummy);
            line = ReadTextLine();
        }

        // references:
        else if (ReadParam(line, prefix_references, dummy, /*preserveWhitespace=*/true))
        {
            // Just store the references unmodified, we don't modify this
            // data anywhere.
            mrefs.push_back(dummy);
            line = ReadTextLine();
        }

        // previous msgid value:
        else if (ReadParam(line, prefix_prev_msgid, dummy))
        {
            msgid_old.Add(dummy);
            line = ReadTextLine();
        }

        // msgctxt:
        else if (ReadParam(line, prefix_msgctxt, dummy))
        {
            has_context = true;
            msgctxt = UnescapeCString(dummy.RemoveLast());
            while (!(line = ReadTextLine()).empty())
            {
                if (line[0u] == _T('\t'))
                    line.Remove(0, 1);
                if (line[0u] == _T('"') && line.Last() == _T('"'))
                {
                    msgctxt += UnescapeCString(line.Mid(1, line.Length() - 2));
                    PossibleWrappedLine();
                }
                else
                    break;
            }
        }

        // msgid:
        else if (ReadParam(line, prefix_msgid, dummy))
        {
            mstr = UnescapeCString(dummy.RemoveLast());
            mlinenum = unsigned(m_textFile->GetCurrentLine() + 1);
            while (!(line = ReadTextLine()).empty())
            {
                if (line[0u] == wxS('\t'))
                    line.Remove(0, 1);
                if (line[0u] == wxS('"') && line.Last() == wxS('"'))
                {
                    mstr += UnescapeCString(line.Mid(1, line.Length() - 2));
                    PossibleWrappedLine();
                }
                else
                    break;
            }
        }

        // msgid_plural:
        else if (ReadParam(line, prefix_msgid_plural, dummy))
        {
            msgid_plural = UnescapeCString(dummy.RemoveLast());
            has_plural = true;
            mlinenum = unsigned(m_textFile->GetCurrentLine() + 1);
            while (!(line = ReadTextLine()).empty())
            {
                if (line[0u] == _T('\t'))
                    line.Remove(0, 1);
                if (line[0u] == _T('"') && line.Last() == _T('"'))
                {
                    msgid_plural += UnescapeCString(line.Mid(1, line.Length() - 2));
                    PossibleWrappedLine();
                }
                else
                    break;
            }
        }

        // msgstr:
        else if (ReadParam(line, prefix_msgstr, dummy))
        {
            if (has_plural)
            {
                wxLogError(_("Broken PO file: singular form msgstr used together with msgid_plural"));
                return false;
            }

            wxString str = UnescapeCString(dummy.RemoveLast());
            while (!(line = ReadTextLine()).empty())
            {
                if (line[0u] == _T('\t'))
                    line.Remove(0, 1);
                if (line[0u] == _T('"') && line.Last() == _T('"'))
                {
                    str += UnescapeCString(line.Mid(1, line.Length() - 2));
                    PossibleWrappedLine();
                }
                else
                    break;
            }
            mtranslations.Add(str);

            bool shouldIgnore = m_ignoreHeader && (mstr.empty() && !has_context);
            if ( shouldIgnore )
            {
                OnIgnoredEntry();
            }
            else
            {
                if (!mstr.empty() && m_ignoreTranslations)
                    mtranslations.clear();

                if (!OnEntry(mstr, wxEmptyString, false,
                             has_context, msgctxt,
                             mtranslations,
                             mflags, mrefs, mcomment, mextractedcomments, msgid_old,
                             mlinenum))
                {
                    return false;
                }
            }

            mcomment = mstr = msgid_plural = msgctxt = mflags = wxEmptyString;
            has_plural = has_context = false;
            mrefs.Clear();
            mextractedcomments.Clear();
            mtranslations.Clear();
            msgid_old.Clear();
        }

        // msgstr[i]:
        else if (ReadParam(line, prefix_msgstr_plural, dummy))
        {
            if (!has_plural)
            {
                wxLogError(_("Broken PO file: plural form msgstr used without msgid_plural"));
                return false;
            }

            wxString idx = dummy.BeforeFirst(wxS(']'));
            wxString label_prefix = prefix_msgstr_plural + idx + wxS("] \"");

            while (ReadParam(line, label_prefix, dummy))
            {
                wxString str = UnescapeCString(dummy.RemoveLast());

                while (!(line=ReadTextLine()).empty())
                {
                    line.Trim(/*fromRight=*/false);
                    if (line[0u] == wxS('"') && line.Last() == wxS('"'))
                    {
                        str += UnescapeCString(line.Mid(1, line.Length() - 2));
                        PossibleWrappedLine();
                    }
                    else
                    {
                        if (ReadParam(line, prefix_msgstr_plural, dummy))
                        {
                            idx = dummy.BeforeFirst(wxS(']'));
                            label_prefix = prefix_msgstr_plural + idx + wxS("] \"");
                        }
                        break;
                    }
                }
                mtranslations.Add(str);
            }

            if (m_ignoreTranslations)
                mtranslations.clear();

            if (!OnEntry(mstr, msgid_plural, true,
                         has_context, msgctxt,
                         mtranslations,
                         mflags, mrefs, mcomment, mextractedcomments, msgid_old,
                         mlinenum))
            {
                return false;
            }

            mcomment = mstr = msgid_plural = msgctxt = mflags = wxEmptyString;
            has_plural = has_context = false;
            mrefs.Clear();
            mextractedcomments.Clear();
            mtranslations.Clear();
            msgid_old.Clear();
        }

        // deleted lines:
        else if (ReadParam(line, prefix_deleted, dummy))
        {
            wxArrayString deletedLines;
            deletedLines.Add(line);
            mlinenum = unsigned(m_textFile->GetCurrentLine() + 1);
            while (!(line = ReadTextLine()).empty())
            {
                // if line does not start with "#~" anymore, stop reading
                if (!ReadParam(line, prefix_deleted, dummy))
                    break;
                // if the line starts with "#~ msgid", we skipped an empty line
                // and it's a new entry, so stop reading too (see bug #329)
                if (ReadParam(line, prefix_deleted_msgid, dummy))
                    break;

                deletedLines.Add(line);
            }

            if (!m_ignoreTranslations)
            {
                if (!OnDeletedEntry(deletedLines,
                                    mflags, mrefs, mcomment, mextractedcomments, mlinenum))
                {
                    return false;
                }
            }

            mcomment = mstr = msgid_plural = mflags = wxEmptyString;
            has_plural = false;
            mrefs.Clear();
            mextractedcomments.Clear();
            mtranslations.Clear();
            msgid_old.Clear();
        }

        // comment:
        else if (line[0u] == wxS('#'))
        {
            bool readNewLine = false;

            while (!line.empty() &&
                    line[0u] == wxS('#') &&
                   (line.Length() < 2 || (line[1u] != wxS(',') && line[1u] != wxS(':') && line[1u] != wxS('.') && line[1u] != wxS('~') )))
            {
                mcomment << line << wxS('\n');
                readNewLine = true;
                line = ReadTextLine();
            }

            if (!readNewLine)
                line = ReadTextLine();
        }

        else
        {
            line = ReadTextLine();
        }
    }

    return true;
}


wxString POCatalogParser::ReadTextLine()
{
    m_previousLineHardWrapped = m_lastLineHardWrapped;
    m_lastLineHardWrapped = false;

    static const wxString msgid_alone(wxS("msgid \"\""));
    static const wxString msgstr_alone(wxS("msgstr \"\""));

    for (;;)
    {
        if (m_textFile->Eof())
            return wxString();

        // read next line and strip insignificant whitespace from it:
        const auto& ln = m_textFile->GetNextLine();
        if (ln.empty())
            continue;

        // gettext tools don't include (extracted) comments in wrapping, so they can't
        // be reliably used to detect file's wrapping either; just skip them.
        if (!ln.starts_with(wxS("#. ")) && !ln.starts_with(wxS("# ")))
        {
            if (ln.ends_with(wxS("\\n\"")))
            {
                // Similarly, lines ending with \n are always wrapped, so skip that too.
                m_lastLineHardWrapped = true;
            }
            else if (ln == msgid_alone || ln == msgstr_alone)
            {
                // The header is always indented like this
                m_lastLineHardWrapped = true;
            }
            else
            {
                // Watch out for lines with too long words that couldn't be wrapped.
                // That "2" is to account for unwrappable comment lines: "#: somethinglong"
                // See https://github.com/vslavik/poedit/issues/135
                auto space = ln.find_last_of(' ');
                if (space != wxString::npos && space > 2)
                {
                    m_detectedLineWidth = std::max(m_detectedLineWidth, (int)ln.size());
                }
            }
        }

        if (wxIsspace(ln[0]) || wxIsspace(ln.Last()))
        {
            auto s = ln.Strip(wxString::both);
            if (!s.empty())
                return s;
        }
        else
        {
            return ln;
        }
    }

    return wxString();
}

int POCatalogParser::GetWrappingWidth() const
{
    if (!m_detectedWrappedLines)
        return POCatalog::NO_WRAPPING;

    return m_detectedLineWidth;
}



class POCharsetInfoFinder : public POCatalogParser
{
    public:
        POCharsetInfoFinder(wxTextFile *f)
                : POCatalogParser(f), m_charset("UTF-8") {}
        wxString GetCharset() const { return m_charset; }

    protected:
        wxString m_charset;

        virtual bool OnEntry(const wxString& msgid,
                             const wxString& /*msgid_plural*/,
                             bool /*has_plural*/,
                             bool has_context,
                             const wxString& /*context*/,
                             const wxArrayString& mtranslations,
                             const wxString& /*flags*/,
                             const wxArrayString& /*references*/,
                             const wxString& /*comment*/,
                             const wxArrayString& /*extractedComments*/,
                             const wxArrayString& /*msgid_old*/,
                             unsigned /*lineNumber*/)
        {
            if (msgid.empty() && !has_context)
            {
                // gettext header:
                Catalog::HeaderData hdr;
                hdr.FromString(mtranslations[0]);
                m_charset = hdr.Charset;
                if (m_charset == "CHARSET")
                    m_charset = "ISO-8859-1";
                return false; // stop parsing
            }
            return true;
        }
};



class POLoadParser : public POCatalogParser
{
    public:
        POLoadParser(POCatalog& c, wxTextFile *f)
              : POCatalogParser(f),
                FileIsValid(false),
                m_catalog(c), m_nextId(1), m_seenHeaderAlready(false) {}

        // true if the file is valid, i.e. has at least some data
        bool FileIsValid;

        Language GetSpecifiedMsgidLanguage()
        {
            auto x_srclang = m_catalog.Header().GetHeader("X-Source-Language");
            if (x_srclang.empty())
                x_srclang = m_catalog.m_header.GetHeader("X-Loco-Source-Locale");
            if (!x_srclang.empty())
            {
                auto parsed = Language::TryParse(str::to_utf8(x_srclang));
                if (parsed.IsValid())
                    return parsed;
            }
            return Language();
        }

    protected:
        POCatalog& m_catalog;

        virtual bool OnEntry(const wxString& msgid,
                             const wxString& msgid_plural,
                             bool has_plural,
                             bool has_context,
                             const wxString& context,
                             const wxArrayString& mtranslations,
                             const wxString& flags,
                             const wxArrayString& references,
                             const wxString& comment,
                             const wxArrayString& extractedComments,
                             const wxArrayString& msgid_old,
                             unsigned lineNumber);

        virtual bool OnDeletedEntry(const wxArrayString& deletedLines,
                                    const wxString& flags,
                                    const wxArrayString& references,
                                    const wxString& comment,
                                    const wxArrayString& extractedComments,
                                    unsigned lineNumber);

        virtual void OnIgnoredEntry() { FileIsValid = true; }

    private:
        int m_nextId;
        bool m_seenHeaderAlready;
};


bool POLoadParser::OnEntry(const wxString& msgid,
                         const wxString& msgid_plural,
                         bool has_plural,
                         bool has_context,
                         const wxString& context,
                         const wxArrayString& mtranslations,
                         const wxString& flags,
                         const wxArrayString& references,
                         const wxString& comment,
                         const wxArrayString& extractedComments,
                         const wxArrayString& msgid_old,
                         unsigned lineNumber)
{
    FileIsValid = true;

    static const wxString MSGCAT_CONFLICT_MARKER("#-#-#-#-#");

    if (msgid.empty() && !has_context)
    {
        if (!m_seenHeaderAlready)
        {
            // gettext header:
            m_catalog.m_header.FromString(mtranslations[0]);
            m_catalog.m_header.Comment = comment;
            for (const auto& s : extractedComments)
                m_catalog.m_header.Comment += "\n#. " + s;
            for (const auto& s : references)
                m_catalog.m_header.Comment += "\n#: " + s;
            if (!flags.empty())
                m_catalog.m_header.Comment += "\n#" + flags;
            m_seenHeaderAlready = true;
        }
        // else: ignore duplicate header in malformed files
    }
    else
    {
        auto d = std::make_shared<POCatalogItem>();
        d->SetId(m_nextId++);
        if (!flags.empty())
            d->SetFlags(flags);
        d->SetString(msgid);
        if (has_plural)
        {
            m_catalog.m_hasPluralItems = true;
            d->SetPluralString(msgid_plural);
        }
        if (has_context)
            d->SetContext(context);
        d->SetTranslations(mtranslations);
        d->SetComment(comment);
        d->SetLineNumber(lineNumber);
        d->SetRawReferences(references);

        for (auto i: extractedComments)
        {
            // Sometimes, msgcat produces conflicts in extracted comments; see the gory details:
            // https://groups.google.com/d/topic/poedit/j41KuvXtVUU/discussion
            // As a workaround, just filter them out.
            // FIXME: Fix this properly... but not using msgcat in the first place
            if (i.starts_with(MSGCAT_CONFLICT_MARKER) && i.ends_with(MSGCAT_CONFLICT_MARKER))
                continue;
            d->AddExtractedComments(i);
        }
        d->SetOldMsgid(msgid_old);
        m_catalog.AddItem(d);
    }
    return true;
}

bool POLoadParser::OnDeletedEntry(const wxArrayString& deletedLines,
                                const wxString& flags,
                                const wxArrayString& /*references*/,
                                const wxString& comment,
                                const wxArrayString& extractedComments,
                                unsigned lineNumber)
{
    FileIsValid = true;

    POCatalogDeletedData d;
    if (!flags.empty()) d.SetFlags(flags);
    d.SetDeletedLines(deletedLines);
    d.SetComment(comment);
    d.SetLineNumber(lineNumber);
    for (size_t i = 0; i < extractedComments.GetCount(); i++)
      d.AddExtractedComments(extractedComments[i]);
    m_catalog.AddDeletedItem(d);

    return true;
}


// ----------------------------------------------------------------------
// POCatalogItem class
// ----------------------------------------------------------------------

wxArrayString POCatalogItem::GetReferences() const
{
    // A line may contain several references, separated by white-space.
    // Traditionally, each reference was in the form "path_name:line_number", but non
    // standard references are sometime used too, including hyperlinks.
    // Filenames that contain spaces are supported - they must be enclosed by Unicode
    // characters U+2068 and U+2069.
    wxArrayString refs;

    for (auto ref = m_references.begin(); ref != m_references.end(); ++ref)
    {
        auto line = ref->Strip(wxString::both);
        wxString buf;

        auto i = line.begin();
        while (i != line.end())
        {
            const wchar_t c = *i;
            if (wxIsspace(c))
            {
                // store reference text encountered so far:
                if (!buf.empty())
                {
                    refs.push_back(buf);
                    buf.clear();
                }
                ++i;
            }
            else if (c == L'\u2068')
            {
                // quoted filename between U+2068 and U+2069:
                ++i;
                while (i != line.end() && *i != L'\u2069')
                {
                    buf += *i;
                    ++i;
                }
                if (i != line.end())
                    ++i; // skip trailing U+2069
            }
            else
            {
                buf += c;
                ++i;
            }
        }

        if (!buf.empty())
            refs.push_back(buf);
    }

    return refs;
}


// ----------------------------------------------------------------------
// POCatalog class
// ----------------------------------------------------------------------

POCatalog::POCatalog(Type type) : Catalog(type)
{
    m_fileCRLF = wxTextFileType_None;
    m_fileWrappingWidth = DEFAULT_WRAPPING;
}

POCatalog::POCatalog(const wxString& po_file, int flags) : Catalog(Type::PO)
{
    m_fileCRLF = wxTextFileType_None;
    m_fileWrappingWidth = DEFAULT_WRAPPING;

    Load(po_file, flags);
}

void POCatalog::PostCreation()
{
    Catalog::PostCreation();
    
    // gettext historically assumes English:
    if (!m_sourceLanguage.IsValid() && !m_sourceIsSymbolicID)
        m_sourceLanguage = Language::English();
}


bool POCatalog::HasCapability(Catalog::Cap cap) const
{
    switch (cap)
    {
        case Cap::Translations:
        case Cap::LanguageSetting:
        case Cap::UserComments:
        case Cap::FuzzyTranslations:
            return m_fileType == Type::PO;
    }
    return false; // silence VC++ warning
}


bool POCatalog::CanLoadFile(const wxString& extension)
{
    return extension == "po" || extension == "pot";
}


wxString POCatalog::GetPreferredExtension() const
{
    switch (m_fileType)
    {
        case Type::PO:
            return "po";
        case Type::POT:
            return "pot";

        default:
            wxFAIL_MSG("not possible here");
            return "po";
    }

    return "po";
}


static inline wxString GetCurrentTimeString()
{
    return wxDateTime::Now().Format("%Y-%m-%d %H:%M%z");
}


void POCatalog::Load(const wxString& po_file, int flags)
{
    wxTextFile f;

    Clear();
    m_fileName = po_file;
    m_header.BasePath = wxEmptyString;

    wxString ext;
    wxFileName::SplitPath(po_file, nullptr, nullptr, &ext);
    if (ext.CmpNoCase("pot") == 0 || (flags & CreationFlag_IgnoreTranslations))
        m_fileType = Type::POT;
    else
        m_fileType = Type::PO;

    /* Load the .po file: */

    if (!f.Open(po_file, wxConvISO8859_1))
    {
        BOOST_THROW_EXCEPTION(Exception(_(L"Couldn’t load the file, it is probably damaged.")));
    }

    {
        wxLogNull null; // don't report parsing errors from here, report them later
        POCharsetInfoFinder charsetFinder(&f);
        charsetFinder.Parse();
        m_header.Charset = charsetFinder.GetCharset();
    }

    f.Close();
    wxCSConv encConv(m_header.Charset);
    if (!f.Open(po_file, encConv))
    {
        BOOST_THROW_EXCEPTION(Exception(_(L"Couldn’t load the file, it is probably damaged.")));
    }

    if (!VerifyFileCharset(f, po_file, m_header.Charset))
    {
        wxLogError(_("There were errors when loading the file. Some data may be missing or corrupted as the result."));
    }

    POLoadParser parser(*this, &f);
    parser.IgnoreHeader(flags & CreationFlag_IgnoreHeader);
    parser.IgnoreTranslations(flags & CreationFlag_IgnoreTranslations);
    if (!parser.Parse())
    {
        BOOST_THROW_EXCEPTION(Exception(_(L"Couldn’t load the file, it is probably damaged.")));
    }

    m_sourceLanguage = parser.GetSpecifiedMsgidLanguage();  // may be, and likely will, invalid

    m_fileCRLF = GetFileCRLFFormat(f);
    m_fileWrappingWidth = parser.GetWrappingWidth();
    wxLogTrace("poedit", "detect line wrapping: %d", m_fileWrappingWidth);

    // If we didn't find any entries, the file must be invalid:
    if (!parser.FileIsValid)
    {
        BOOST_THROW_EXCEPTION(Exception(_(L"Couldn’t load the file, it is probably damaged.")));
    }

    f.Close();

    FixupCommonIssues();

    if ( flags & CreationFlag_IgnoreHeader )
        CreateNewHeader();
}


void POCatalog::FixupCommonIssues()
{
    if (m_header.Project == "PACKAGE VERSION")
        m_header.Project.clear();

    // In PHP use, strings with % (typically: 100%) get frequently mis-identified as php-format, because the
    // format string allows space, so e.g. "100% complete" has a valid "% c" format flag in it. Work around
    // this by removing the flag ourselves, as translators can rarely influence it:
    for (auto& i: items())
    {
        if (i->GetFormatFlag() == "php")
        {
            auto s = i->GetRawString();
            if (s.Contains(wxS("% ")) && !s.Contains(wxS("%% ")))
            {
                auto poi = std::dynamic_pointer_cast<POCatalogItem>(i);
                poi->m_moreFlags.Replace("php-format", "no-php-format");
            }
        }
    }

    // All the following fixups are specific to POs and should *not* be done in POTs:
    if (m_fileType == Type::POT)
        return;

    if (m_header.GetHeader("Language-Team") == "LANGUAGE <LL@li.org>")
    {
        m_header.DeleteHeader("Language-Team");
        m_header.LanguageTeam.clear();
    }

    if (m_header.GetHeader("Last-Translator") == "FULL NAME <EMAIL@ADDRESS>")
    {
        m_header.DeleteHeader("Last-Translator");
        m_header.Translator.clear();
        m_header.TranslatorEmail.clear();
    }

    wxString pluralForms = m_header.GetHeader("Plural-Forms");

    if (pluralForms == "nplurals=INTEGER; plural=EXPRESSION;") // default invalid value
        pluralForms = "";

    if (!pluralForms.empty())
    {
        if (!pluralForms.ends_with(";"))
        {
            pluralForms += ";";
            m_header.SetHeader("Plural-Forms", pluralForms);
        }
    }
    else
    {
        // Auto-fill default plural form if it is missing:
        if (m_header.Lang.IsValid() && HasPluralItems())
        {
            pluralForms = m_header.Lang.DefaultPluralFormsExpr().str();
            if (!pluralForms.empty())
                m_header.SetHeader("Plural-Forms", pluralForms);
        }
    }
}


void POCatalog::Clear()
{
    // Catalog base class fields:
    m_items.clear();

    // PO-specific fields:
    m_deletedItems.clear();
}


// misc file-saving helpers
namespace
{

inline bool CanEncodeStringToCharset(const wxString& s, wxMBConv& conv)
{
    if (s.empty())
        return true;
    const wxCharBuffer converted(s.mb_str(conv));
    if ( converted.length() == 0 )
        return false;
    return true;
}

bool CanEncodeToCharset(const wxTextBuffer& f, const wxString& charset)
{
    if (charset.Lower() == "utf-8" || charset.Lower() == "utf8")
        return true;

    wxCSConv conv(charset);

    const size_t lines = f.GetLineCount();
    for ( size_t i = 0; i < lines; i++ )
    {
        if ( !CanEncodeStringToCharset(f.GetLine(i), conv) )
            return false;
    }

    return true;
}

template<typename Func>
inline void SplitIntoLines(const wxString& text, Func&& f)
{
    if (text.empty())
        return;

    wxString::const_iterator last = text.begin();
    for (wxString::const_iterator i = text.begin(); i != text.end(); ++i)
    {
        if (*i == '\n')
        {
            f(wxString(last, i), false);
            last = i + 1;
        }
    }

    if (last != text.end())
        f(wxString(last, text.end()), true);
}

void SaveMultiLines(wxTextBuffer &f, const wxString& text)
{
    SplitIntoLines(text, [&f](wxString&& s, bool)
    {
        f.AddLine(s);
    });
}

/** Adds \n characters as necessary for good-looking output
 */
wxString FormatStringForFile(const wxString& text)
{
    wxString s;
    s.reserve(text.length() + 16);

    static wxString quoted_newline(wxS("\"\n\""));

    SplitIntoLines(text, [&s](wxString&& piece, bool last)
    {
        if (!s.empty())
            s += quoted_newline;
        if (!last)
            piece += '\n';
        EscapeCStringInplace(piece);
        s += piece;
    });

    return s;
}

} // anonymous namespace


#ifdef __WXOSX__

@interface CompiledMOFilePresenter : NSObject<NSFilePresenter>
@property (atomic, copy) NSURL *presentedItemURL;
@property (atomic, copy) NSURL *primaryPresentedItemURL;
@end

@implementation CompiledMOFilePresenter
- (NSOperationQueue *)presentedItemOperationQueue {
     return [NSOperationQueue mainQueue];
}
@end

#endif // __WXOSX__


bool POCatalog::Save(const wxString& po_file, bool save_mo,
                     ValidationResults& validation_results, CompilationStatus& mo_compilation_status)
{
    mo_compilation_status = CompilationStatus::NotDone;

    if ( wxFileExists(po_file) && !wxFile::Access(po_file, wxFile::write) )
    {
        wxLogError(_(L"File “%s” is read-only and cannot be saved.\nPlease save it under different name."),
                   po_file.c_str());
        return false;
    }

    // Update information about last modification time. But if the header
    // was empty previously, the author apparently doesn't want this header
    // set, so don't mess with it. See https://sourceforge.net/tracker/?func=detail&atid=389156&aid=1900298&group_id=27043
    // for motivation:
    auto currentTime = GetCurrentTimeString();
    switch (m_fileType)
    {
        case Type::PO:
            if ( !m_header.RevisionDate.empty() )
                m_header.RevisionDate = currentTime;
            break;
        case Type::POT:
            if ( m_fileType == Type::POT && !m_header.CreationDate.empty() )
                m_header.CreationDate = currentTime;
            break;

        default:
            wxFAIL_MSG("not possible here");
            break;
    }

    TempOutputFileFor po_file_temp_obj(po_file);
    const wxString po_file_temp = po_file_temp_obj.FileName();

    wxTextFileType outputCrlf = GetDesiredCRLFFormat(m_fileCRLF);
    // Save into Unix line endings first and only if Windows is required,
    // reformat the file later. This is because msgcat cannot handle DOS
    // input particularly well.

    if ( !DoSaveOnly(po_file_temp, wxTextFileType_Unix) )
    {
        wxLogError(_(L"Couldn’t save file %s."), po_file.c_str());
        return false;
    }

    try
    {
        validation_results = Validate(/*fileWithSameContent=*/po_file_temp);
    }
    catch (...)
    {
        // DoValidate may fail catastrophically if app bundle is damaged, but
        // that shouldn't prevent Poedit from trying to save user's file.
        wxLogError("%s", DescribeCurrentException());
    }

    // Now that the file was written, run msgcat to re-format it according
    // to the usual format. This is a (barely) passable fix for #25 until
    // proper preservation of formatting is implemented.

    bool msgcat_ok = false;
    {
        wxArrayString args { "msgcat", "--force-po" };

        int wrapping = DEFAULT_WRAPPING;
        if (wxConfig::Get()->ReadBool("keep_crlf", true))
            wrapping = m_fileWrappingWidth;

        if (wrapping == DEFAULT_WRAPPING)
        {
            if (wxConfig::Get()->ReadBool("wrap_po_files", true))
            {
                wrapping = (int)wxConfig::Get()->ReadLong("wrap_po_files_width", 79);
            }
            else
            {
                wrapping = NO_WRAPPING;
            }
        }

        if (wrapping == NO_WRAPPING)
            args.push_back("--no-wrap");
        else if (wrapping != DEFAULT_WRAPPING)
            args.push_back(wxString::Format("--width=%d", wrapping));

        TempOutputFileFor po_file_temp2_obj(po_file_temp);
        const wxString po_file_temp2 = po_file_temp2_obj.FileName();

        args.push_back("-o");
        args.push_back(po_file_temp2);
        args.push_back(po_file_temp);

        wxLogTrace("poedit", "formatting file with %s", wxJoin(args, ' '));

        // Ignore msgcat errors output (but not exit code), because it
        //   a) complains about things DoValidate() already complained above
        //   b) issues warnings about source-extraction things (e.g. using non-ASCII
        //      msgids) that, while correct, are not something a *translator* can
        //      do anything about.
        GettextRunner runner;
        msgcat_ok = runner.run_sync(args) && wxFileExists(po_file_temp2);

        // msgcat always outputs Unix line endings, so we need to reformat the file
        if (msgcat_ok && outputCrlf == wxTextFileType_Dos)
        {
            wxCSConv conv(m_header.Charset);
            wxTextFile finalFile(po_file_temp2);
            if (finalFile.Open(conv))
                finalFile.Write(outputCrlf, conv);
        }

        if (!TempOutputFileFor::ReplaceFile(po_file_temp2, po_file))
            msgcat_ok = false;
    }

    if ( msgcat_ok )
    {
        wxRemoveFile(po_file_temp);
    }
    else
    {
        if ( !po_file_temp_obj.Commit() )
        {
            wxLogError(_(L"Couldn’t save file %s."), po_file.c_str());
        }
        else
        {
            // Only shows msgcat's failure warning if we don't also get
            // validation errors, because if we do, the cause is likely the
            // same.
            if ( !validation_results.errors )
            {
                wxLogWarning(_("There was a problem formatting the file nicely (but it was saved all right)."));
            }
        }
    }

    
    /* If the user wants it, compile .mo file right now: */

    bool compileMO = save_mo;
    if (!wxConfig::Get()->Read("compile_mo", (long)true))
        compileMO = false;

    if (m_fileType == Type::PO && compileMO)
    {
        const wxString mo_file = wxFileName::StripExtension(po_file) + ".mo";
        TempOutputFileFor mo_file_temp_obj(mo_file);
        const wxString mo_file_temp = mo_file_temp_obj.FileName();

        {
            // Ignore msgfmt errors output (but not exit code), because it
            // complains about things DoValidate() already complained above.
            if (GettextRunner().run_sync("msgfmt", "-o", mo_file_temp, CliSafeFileName(po_file)))
            {
                mo_compilation_status = CompilationStatus::Success;
            }
            else
            {
                // Don't report errors, they were reported as part of validation
                // step above.  Notice that we run msgfmt *without* the -c flag
                // here to create the MO file in as many cases as possible, even if
                // it has some errors.
                //
                // Still, msgfmt has the ugly habit of sometimes returning non-zero
                // exit code, reporting "fatal errors" and *still* producing a usable
                // .mo file. If this happens, don't pretend the file wasn't created.
                if (wxFileName::FileExists(mo_file_temp))
                    mo_compilation_status = CompilationStatus::Success;
                else
                    mo_compilation_status = CompilationStatus::Error;
            }
        }

        // Move the MO from temporary location to the final one, if it was created
        if (mo_compilation_status == CompilationStatus::Success)
        {
#ifdef __WXOSX__
            NSURL *mofileUrl = [NSURL fileURLWithPath:str::to_NS(mo_file)];
            NSURL *mofiletempUrl = [NSURL fileURLWithPath:str::to_NS(mo_file_temp)];

            CompiledMOFilePresenter *presenter = [CompiledMOFilePresenter new];
            presenter.presentedItemURL = mofileUrl;
            presenter.primaryPresentedItemURL = [NSURL fileURLWithPath:str::to_NS(po_file)];
            [NSFileCoordinator addFilePresenter:presenter];

            NSFileCoordinator *coo = [[NSFileCoordinator alloc] initWithFilePresenter:presenter];
            [coo coordinateWritingItemAtURL:mofileUrl options:NSFileCoordinatorWritingForReplacing error:nil byAccessor:^(NSURL *newURL) {
                NSURL *resultingUrl;
                BOOL ok = [[NSFileManager defaultManager] replaceItemAtURL:newURL
                                                             withItemAtURL:mofiletempUrl
                                                            backupItemName:nil
                                                                   options:0
                                                          resultingItemURL:&resultingUrl
                                                                     error:nil];
                if (!ok)
                {
                    wxLogError(_(L"Couldn’t save file %s."), mo_file.c_str());
                    mo_compilation_status = CompilationStatus::Error;
                }
            }];

            [NSFileCoordinator removeFilePresenter:presenter];
#else // !__WXOSX__
            if ( !mo_file_temp_obj.Commit() )
            {
                wxLogError(_(L"Couldn’t save file %s."), mo_file.c_str());
                mo_compilation_status = CompilationStatus::Error;
            }
#endif // __WXOSX__/!__WXOSX__
        }
    }

    SetFileName(po_file);

    return true;
}


std::string POCatalog::SaveToBuffer()
{
    class StringSerializer : public wxMemoryText
    {
    public:
        bool OnWrite(wxTextFileType typeNew, const wxMBConv& conv) override
        {
            size_t cnt = GetLineCount();
            for (size_t n = 0; n < cnt; n++)
            {
                auto ln = GetLine(n) +
                          GetEOL(typeNew == wxTextFileType_None ? GetLineType(n) : typeNew);
                auto buf = ln.mb_str(conv);
                buffer.append(buf.data(), buf.length());
            }
            return true;
        }

        std::string buffer;
    };

    StringSerializer f;

    if (!DoSaveOnly(f, wxTextFileType_Unix))
        return std::string();
    return f.buffer;
}


bool POCatalog::CompileToMO(const wxString& mo_file,
                            ValidationResults& validation_results,
                            CompilationStatus& mo_compilation_status)
{
    mo_compilation_status = CompilationStatus::NotDone;

    TempDirectory tmpdir;
    if ( !tmpdir.IsOk() )
        return false;

    wxFileName mofn(mo_file);
    wxString po_file_temp = tmpdir.CreateFileName(mofn.GetName() + ".po");

    if ( !DoSaveOnly(po_file_temp, wxTextFileType_Unix) )
    {
        wxLogError(_(L"Couldn’t save file %s."), po_file_temp.c_str());
        return false;
    }

    validation_results = Validate(/*fileWithSameContent=*/po_file_temp);

    TempOutputFileFor mo_file_temp_obj(mo_file);
    const wxString mo_file_temp = mo_file_temp_obj.FileName();

    GettextRunner runner;
    runner.run_sync("msgfmt", "-o", mo_file_temp, po_file_temp);

    // Ignore msgfmt errors output (but not exit code), because it
    // complains about things DoValidate() already complained above.
    //
    // Don't check return code either:
    // msgfmt has the ugly habit of sometimes returning non-zero
    // exit code, reporting "fatal errors" and *still* producing a usable
    // .mo file. If this happens, don't pretend the file wasn't created.
    if (!wxFileName::FileExists(mo_file_temp))
    {
        mo_compilation_status = CompilationStatus::Error;
        return false;
    }
    else
    {
        mo_compilation_status = CompilationStatus::Success;
    }

    if ( !mo_file_temp_obj.Commit() )
    {
        wxLogError(_(L"Couldn’t save file %s."), mo_file.c_str());
        return false;
    }

    return true;
}




bool POCatalog::DoSaveOnly(const wxString& po_file, wxTextFileType crlf)
{
    wxTextFile f;
    if (!f.Create(po_file))
        return false;

    return DoSaveOnly(f, crlf);
}

bool POCatalog::DoSaveOnly(wxTextBuffer& f, wxTextFileType crlf)
{
    const bool isPOT = m_fileType == Type::POT;

    /* Save .po file: */
    if (!m_header.Charset || m_header.Charset == "CHARSET")
        m_header.Charset = "UTF-8";

    SaveMultiLines(f, m_header.Comment);
    if (isPOT)
        f.AddLine(wxS("#, fuzzy"));
    f.AddLine(wxS("msgid \"\""));
    f.AddLine(wxS("msgstr \"\""));
    wxString pohdr = wxString(wxS("\"")) + m_header.ToString(wxS("\"\n\""));
    pohdr.RemoveLast();
    SaveMultiLines(f, pohdr);
    f.AddLine(wxEmptyString);

    auto pluralsCount = GetPluralFormsCount();

    for (auto& data_: m_items)
    {
        auto data = std::static_pointer_cast<POCatalogItem>(data_);

        data->SetLineNumber(int(f.GetLineCount()+1));
        SaveMultiLines(f, data->GetComment());
        for (unsigned i = 0; i < data->GetExtractedComments().GetCount(); i++)
        {
            if (data->GetExtractedComments()[i].empty())
              f.AddLine(wxS("#."));
            else
              f.AddLine(wxS("#. ") + data->GetExtractedComments()[i]);
        }
        for (unsigned i = 0; i < data->GetRawReferences().GetCount(); i++)
            f.AddLine(wxS("#: ") + data->GetRawReferences()[i]);
        wxString dummy = data->GetFlags();
        if (!dummy.empty())
            f.AddLine(wxS("#") + dummy);
        for (unsigned i = 0; i < data->GetOldMsgidRaw().GetCount(); i++)
            f.AddLine(wxS("#| ") + data->GetOldMsgidRaw()[i]);
        if ( data->HasContext() )
        {
            SaveMultiLines(f, wxS("msgctxt \"") + FormatStringForFile(data->GetContext()) + wxS("\""));
        }
        dummy = FormatStringForFile(data->GetRawString());
        SaveMultiLines(f, wxS("msgid \"") + dummy + wxS("\""));
        if (data->HasPlural())
        {
            dummy = FormatStringForFile(data->GetRawPluralString());
            SaveMultiLines(f, wxS("msgid_plural \"") + dummy + wxS("\""));

            for (unsigned i = 0; i < pluralsCount; i++)
            {
                dummy = FormatStringForFile(data->GetTranslation(i));
                wxString hdr = wxString::Format(wxS("msgstr[%u] \""), i);
                SaveMultiLines(f, hdr + dummy + wxS("\""));
            }
        }
        else
        {
            if (isPOT)
            {
                f.AddLine(wxS("msgstr \"\""));
            }
            else
            {
                dummy = FormatStringForFile(data->GetTranslation());
                SaveMultiLines(f, wxS("msgstr \"") + dummy + wxS("\""));
            }
        }
        f.AddLine(wxEmptyString);
    }

    // Write back deleted items in the file so that they're not lost
    for (unsigned itemIdx = 0; itemIdx < m_deletedItems.size(); itemIdx++)
    {
        if ( itemIdx != 0 )
            f.AddLine(wxEmptyString);

        POCatalogDeletedData& deletedItem = m_deletedItems[itemIdx];
        deletedItem.SetLineNumber(int(f.GetLineCount()+1));
        SaveMultiLines(f, deletedItem.GetComment());
        for (unsigned i = 0; i < deletedItem.GetExtractedComments().GetCount(); i++)
            f.AddLine(wxS("#. ") + deletedItem.GetExtractedComments()[i]);
        for (unsigned i = 0; i < deletedItem.GetRawReferences().GetCount(); i++)
            f.AddLine(wxS("#: ") + deletedItem.GetRawReferences()[i]);
        wxString dummy = deletedItem.GetFlags();
        if (!dummy.empty())
            f.AddLine(wxS("#") + dummy);

        for (size_t j = 0; j < deletedItem.GetDeletedLines().GetCount(); j++)
            f.AddLine(deletedItem.GetDeletedLines()[j]);
    }

    if (!CanEncodeToCharset(f, m_header.Charset))
    {
#if wxUSE_GUI
        wxString msg;
        msg.Printf(_(L"The file couldn’t be saved in “%s” charset as specified in translation settings.\n\nIt was saved in UTF-8 instead and the setting was modified accordingly."),
                   m_header.Charset.c_str());
        wxMessageBox(msg, _("Error saving file"),
                     wxOK | wxICON_EXCLAMATION);
#endif
        m_header.Charset = "UTF-8";

        // Re-do the save again because we modified a header:
        f.Clear();
        return DoSaveOnly(f, crlf);
    }

    // Otherwise everything can be safely saved:
    return f.Write(crlf, wxCSConv(m_header.Charset));
}

void POCatalog::SetLanguage(Language lang)
{
    Catalog::SetLanguage(lang);
    // don't add unneeded header, but always update it if already present:
    if (HasPluralItems() || m_header.HasHeader("Plural-Forms"))
        m_header.SetHeaderNotEmpty("Plural-Forms", lang.DefaultPluralFormsExpr().str());
}

unsigned POCatalog::GetPluralFormsCount() const
{
    return std::max(GetCountFromPluralFormsHeader(m_header), Catalog::GetPluralFormsCount());
}

bool POCatalog::HasWrongPluralFormsCount() const
{
    unsigned count = 0;

    for (auto& i: m_items)
    {
        count = std::max(count, i->GetPluralFormsCount());
    }

    if ( count == 0 )
        return false; // nothing translated, so we can't tell

    // if 'count' is less than the count from header, it may simply mean there
    // are untranslated strings
    if ( count > GetCountFromPluralFormsHeader(m_header) )
        return true;

    return false;
}


bool POCatalog::HasDuplicateItems() const
{
    typedef std::pair<wxString, wxString> MsgId;
    std::set<MsgId> ids;
    for (auto& item: m_items)
    {
        if (!ids.emplace(std::make_pair(item->GetContext(), item->GetRawString())).second)
            return true;
    }
    return false;
}

bool POCatalog::FixDuplicateItems()
{
    auto oldname = m_fileName;

    TempDirectory tmpdir;
    if ( !tmpdir.IsOk() )
        return false;

    wxString ext;
    wxFileName::SplitPath(m_fileName, nullptr, nullptr, &ext);
    wxString po_file_temp = tmpdir.CreateFileName("catalog." + ext);
    wxString po_file_fixed = tmpdir.CreateFileName("fixed." + ext);

    if ( !DoSaveOnly(po_file_temp, wxTextFileType_Unix) )
    {
        wxLogError(_(L"Couldn’t save file %s."), po_file_temp.c_str());
        return false;
    }

    GettextRunner().run_sync("msguniq", "-o", CliSafeFileName(po_file_fixed), CliSafeFileName(po_file_temp));

    if (!wxFileName::FileExists(po_file_fixed))
        return false;

    Load(po_file_fixed);
    m_fileName = oldname;
    PostCreation();

    return true;
}


Catalog::ValidationResults POCatalog::Validate(const wxString& fileWithSameContent)
{
    ValidationResults res = Catalog::Validate(fileWithSameContent);

    if (!HasCapability(Catalog::Cap::Translations))
        return res;  // no errors in POT files

    if (!fileWithSameContent.empty())
    {
        ValidateWithMsgfmt(res, fileWithSameContent);
    }
    else
    {
        TempDirectory tmpdir;
        if ( !tmpdir.IsOk() )
            return res;

        wxString tmp_po = tmpdir.CreateFileName("validated.po");
        if ( !DoSaveOnly(tmp_po, wxTextFileType_Unix) )
            return res;

        ValidateWithMsgfmt(res, tmp_po);
    }

    return res;
}

void POCatalog::ValidateWithMsgfmt(Catalog::ValidationResults& res, const wxString& po_file)
{
    GettextRunner gtr;
    auto output = gtr.run_sync("msgfmt", "-o", "/dev/null", "-c", CliSafeFileName(po_file));
    auto errors = gtr.parse_stderr(output);

    for (auto& i: errors.items)
    {
        if (i.has_location())
        {
            auto item = FindItemByLine(i.line);
            if (item)
            {
                res.errors++;
                item->SetIssue(CatalogItem::Issue::Error, i.text);
            }
        }
        // else: ignore msgfmt output w/o a location because msgfmt outputs status information
        //       (e.g. "N errors found") to stderr too
    }
}


bool POCatalog::UpdateFromPOT(const wxString& pot_file, bool replace_header)
{
    try
    {
        POCatalogPtr pot = std::dynamic_pointer_cast<POCatalog>(Catalog::Create(pot_file, CreationFlag_IgnoreTranslations));
        return UpdateFromPOT(pot, replace_header);
    }
    catch (...) // FIXME
    {
        wxLogError(_(L"“%s” is not a valid POT file."), pot_file.c_str());
        return false;
    }
}

bool POCatalog::UpdateFromPOT(POCatalogPtr pot, bool replace_header)
{
    switch (m_fileType)
    {
        case Type::PO:
        {
            if (!Merge(pot))
                return false;
            break;
        }
        case Type::POT:
        {
            m_items = pot->m_items;
            m_sourceLanguage = pot->m_sourceLanguage;
            m_sourceIsSymbolicID = pot->m_sourceIsSymbolicID;
            m_hasPluralItems = pot->m_hasPluralItems;
            break;
        }

        default:
            wxFAIL_MSG("not possible here");
            break;
    }

    if (replace_header)
        CreateNewHeader(pot->Header());

    return true;
}

POCatalogPtr POCatalog::CreateFromPOT(POCatalogPtr pot)
{
    POCatalogPtr c(new POCatalog(Type::PO));
    if (c->UpdateFromPOT(pot, /*replace_header=*/true))
        return c;
    else
        return nullptr;
}

bool POCatalog::Merge(const POCatalogPtr& refcat)
{
    wxString oldname = m_fileName;
    wxON_BLOCK_EXIT_SET(m_fileName, oldname);

    TempDirectory tmpdir;
    if ( !tmpdir.IsOk() )
        return false;

    wxString tmp1 = tmpdir.CreateFileName("ref.pot");
    wxString tmp2 = tmpdir.CreateFileName("input.po");
    wxString tmp3 = tmpdir.CreateFileName("output.po");

    refcat->DoSaveOnly(tmp1, wxTextFileType_Unix);
    DoSaveOnly(tmp2, wxTextFileType_Unix);

    std::vector<wxString> args { "msgmerge", "-q", "--force-po", "--previous" };
    if (Config::MergeBehavior() == Merge_None)
    {
        args.push_back("--no-fuzzy-matching");
    }

    args.push_back("-o");
    args.push_back(tmp3);
    args.push_back(tmp2);
    args.push_back(tmp1);

    GettextRunner runner;
    auto output = runner.run_sync(args);
    // FIXME: Don't do that here, report as part of return value instead
    runner.parse_stderr(output).log_errors();

    if (output)
    {
        const wxString charset = m_header.Charset;

        Load(tmp3);
        m_fileName = oldname;
        PostCreation();

        // msgmerge doesn't always preserve the charset, it tends to pick
        // the most generic one of the charsets used, so if we are merging with
        // UTF-8 catalog, it will become UTF-8. Some people hate this.
        m_header.Charset = charset;
    }

    return (bool)output;
}
