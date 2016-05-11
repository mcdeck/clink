// Copyright (c) 2015 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "win_terminal.h"

#include <core/array.h>
#include <core/base.h>
#include <core/log.h>
#include <core/settings.h>
#include <core/str_iter.h>

//------------------------------------------------------------------------------
static setting_bool g_altgr(
    "terminal.altgr",
    "Support Windows' Ctrl-Alt substitute for AltGr",
    "Windows provides Ctrl-Alt as a substitute for AltGr, historically to\n"
    "support keyboards with no AltGr key. This may collide with some of\n"
    "Readline's bindings.",
    true);

static setting_bool g_ansi(
    "terminal.ansi",
    "Enables basic ANSI escape code support",
    "When printing the prompt, Clink has basic built-in support for SGR\n"
    "ANSI escape codes to control the text colours. This is automatically\n"
    "disabled if a third party tool is detected that also provides this\n"
    "facility. It can also be disabled by setting this to 0.",
    true);



//------------------------------------------------------------------------------
static unsigned g_last_buffer_size = 0;
void            on_terminal_resize(); // MODE4



//------------------------------------------------------------------------------
inline void win_terminal_in::begin()
{
    m_buffer_count = 0;

    m_stdin = GetStdHandle(STD_INPUT_HANDLE);

    // Clear 'processed input' flag so key presses such as Ctrl-C and Ctrl-S
    // aren't swallowed. We also want events about window size changes.
    GetConsoleMode(m_stdin, &m_prev_mode);
    SetConsoleMode(m_stdin, ENABLE_WINDOW_INPUT);
}

//------------------------------------------------------------------------------
inline void win_terminal_in::end()
{
    SetConsoleMode(m_stdin, m_prev_mode);
    m_stdin = nullptr;
}

//------------------------------------------------------------------------------
inline void win_terminal_in::select()
{
    if (!m_buffer_count)
        read_console();
}

//------------------------------------------------------------------------------
inline int win_terminal_in::read()
{
    // MODE4 : need protocol for no input available

    int c = pop();
    if (c != 0xff)
        return c;
    
    return 0x04; // EOT; "should never happen"
}

//------------------------------------------------------------------------------
void win_terminal_in::read_console()
{
    // Check for a new buffer size for simulated SIGWINCH signals.
// MODE4
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        HANDLE handle_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(handle_stdout, &csbi);

        DWORD i = (csbi.dwSize.X << 16);
        i |= (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
        if (!g_last_buffer_size || g_last_buffer_size != i)
        {
            if (g_last_buffer_size)
                on_terminal_resize();
    
            g_last_buffer_size = i;
            read_console();
            return;
        }
    }
// MODE4

    // Fresh read from the console.
    DWORD unused;
    INPUT_RECORD record;
    ReadConsoleInputW(m_stdin, &record, 1, &unused);
    if (record.EventType != KEY_EVENT)
    {
        read_console();
        return;
    }

// MODE4
    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
    {
        on_terminal_resize();

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        HANDLE handle_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(handle_stdout, &csbi);

        g_last_buffer_size = (csbi.dwSize.X << 16) | csbi.dwSize.Y;
        read_console();
        return;
    }
// MODE4

    const KEY_EVENT_RECORD* key = &record.Event.KeyEvent;
    int key_char = key->uChar.UnicodeChar;
    int key_vk = key->wVirtualKeyCode;
    int key_sc = key->wVirtualScanCode;
    int key_flags = key->dwControlKeyState;

    if (key->bKeyDown == FALSE)
    {
        // Some times conhost can send through ALT codes, with the resulting
        // Unicode code point in the Alt key-up event.
        if (key_vk == VK_MENU && key_char)
        {
            push(key_char);
            return;
        }

        read_console();
        return;
    }

    // Windows supports an AltGr substitute which we check for here. As it
    // collides with Readline mappings Clink's support can be disabled.
    int altgr_sub;
    altgr_sub = !!(key_flags & LEFT_ALT_PRESSED);
    altgr_sub &= !!(key_flags & (LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED));
    altgr_sub &= !!key_char;

    if (altgr_sub && !g_altgr.get())
    {
        altgr_sub = 0;
        key_char = 0;
    }

    int alt = 0;
    if (!altgr_sub)
        alt = !!(key_flags & (LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED));

    // No Unicode character? Then some post-processing is required to make the
    // output compatible with whatever standard Linux terminals adhere to and
    // that which Readline expects.
    static const int CTRL_PRESSED = LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED;
    if (key_char == 0)
    {
        // The numpad keys such as PgUp, End, etc. don't come through with the
        // ENHANCED_KEY flag set so we'll infer it here.
        static const int enhanced_vks[] = {
            VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_HOME, VK_END,
            VK_INSERT, VK_DELETE, VK_PRIOR, VK_NEXT,
        };

        for (int i = 0; i < sizeof_array(enhanced_vks); ++i)
        {
            if (key_vk == enhanced_vks[i])
            {
                key_flags |= ENHANCED_KEY;
                break;
            }
        }

        // Differentiate enhanced keys depending on modifier key state. MSVC's
        // runtime does something similar. Slightly non-standard.
        if (key_flags & ENHANCED_KEY)
        {
            static const int mod_map[][5] =
            {
                // j---->
                // Scan Nrml Shft
                {  'H', 'A', 'a'  }, // up      i
                {  'P', 'B', 'b'  }, // down    |
                {  'K', 'D', 'd'  }, // left    |
                {  'M', 'C', 'c'  }, // right   v
                {  'R', '2', 'w'  }, // insert
                {  'S', '3', 'e'  }, // delete
                {  'G', '1', 'q'  }, // home
                {  'O', '4', 'r'  }, // end
                {  'I', '5', 't'  }, // pgup
                {  'Q', '6', 'y'  }, // pgdn
            };

            for (int i = 0; i < sizeof_array(mod_map); ++i)
            {
                if (mod_map[i][0] != key_sc)
                    continue;

                int j = 1 + !!(key_flags & SHIFT_PRESSED);

                push(0x1b);
                push((key_flags & CTRL_PRESSED) ? 'O' : '[');
                push(mod_map[i][j]);
                return;
            }

            read_console();
            return;
        }
        else if (!(key_flags & CTRL_PRESSED))
        {
            read_console();
            return;
        }

        // This builds Ctrl-<key> map to match that as described by Readline's
        // source for the emacs/vi keymaps.
        #define CONTAINS(l, r) (unsigned)(key_vk - l) <= (r - l)
        else if (CONTAINS('A', 'Z'))    key_vk -= 'A' - 1;
        else if (CONTAINS(0xdb, 0xdd))  key_vk -= 0xdb - 0x1b;
        else if (key_vk == 0x32)        key_vk = 0;
        else if (key_vk == 0x36)        key_vk = 0x1e;
        else if (key_vk == 0xbd)        key_vk = 0x1f;
        else                            return read_console();
        #undef CONTAINS

        push(key_char);
        return;
    }

    // Special case for shift-tab.
    if (key_char == '\t' && !m_buffer_count && (key_flags & SHIFT_PRESSED))
    {
        push(0x1b);
        push('[');
        push('Z');
        return;
    }

    // Include an ESC character in the input stream if Alt is pressed.
    if (alt)
        push(0x1b);

    push(key_char);
}

//------------------------------------------------------------------------------
void win_terminal_in::push(unsigned int value)
{
    static const unsigned char mask = sizeof_array(m_buffer) - 1;

    if (m_buffer_count >= sizeof_array(m_buffer))
        return;

    int index = m_buffer_head + m_buffer_count;

    if (value < 0x80)
    {
        m_buffer[index & mask] = value;
        ++m_buffer_count;
        return;
    }

    wchar_t wc[2] = { (wchar_t)value, 0 };
    char utf8[8];
    int n = to_utf8(utf8, sizeof_array(utf8), wc);
    if (n <= mask - m_buffer_count)
        for (int i = 0; i < n; ++i, ++index)
            m_buffer[index & mask] = utf8[i];

    m_buffer_count += n;
}

//------------------------------------------------------------------------------
inline unsigned char win_terminal_in::pop()
{
    if (!m_buffer_count)
        return 0xff;

    unsigned char value = m_buffer[m_buffer_head];

    --m_buffer_count;
    m_buffer_head = (m_buffer_head + 1) & (sizeof_array(m_buffer) - 1);

    return value;
}



//------------------------------------------------------------------------------
inline void win_terminal_out::begin()
{
    m_stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_stdout, &csbi);
    m_default_attr = csbi.wAttributes & 0xff;
    m_attr = m_default_attr;

    GetConsoleMode(m_stdout, &m_prev_mode);
}

//------------------------------------------------------------------------------
inline void win_terminal_out::end()
{
    SetConsoleMode(m_stdout, m_prev_mode);
    SetConsoleTextAttribute(m_stdout, m_default_attr);

    m_stdout = nullptr;
}

//------------------------------------------------------------------------------
void win_terminal_out::write(const char* chars, int length)
{
    str_iter iter(chars, length);
    while (length > 0)
    {
        wchar_t wbuf[256];
        int n = min<int>(sizeof_array(wbuf), length + 1);
        n = to_utf16(wbuf, n, iter);

        write(wbuf, n);

        n = int(iter.get_pointer() - chars);
        length -= n;
        chars += n;
    }
}

//------------------------------------------------------------------------------
inline void win_terminal_out::write(const wchar_t* chars, int length)
{
    DWORD written;
    WriteConsoleW(m_stdout, chars, length, &written, nullptr);
}

//------------------------------------------------------------------------------
inline void win_terminal_out::flush()
{
    // When writing to the console conhost.exe will restart the cursor blink
    // timer and hide it which can be disorientating, especially when moving
    // around a line. The below will make sure it stays visible.
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_stdout, &csbi);
    SetConsoleCursorPosition(m_stdout, csbi.dwCursorPosition);
}

//------------------------------------------------------------------------------
inline int win_terminal_out::get_columns() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_stdout, &csbi);
    return csbi.dwSize.X;
}

//------------------------------------------------------------------------------
inline int win_terminal_out::get_rows() const
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(m_stdout, &csbi);
    return (csbi.srWindow.Bottom - csbi.srWindow.Top) + 1;
}

//------------------------------------------------------------------------------
inline unsigned char win_terminal_out::get_default_attr() const
{
    return m_default_attr;
}

//------------------------------------------------------------------------------
inline unsigned char win_terminal_out::get_attr() const
{
    return m_attr;
}

//------------------------------------------------------------------------------
inline void win_terminal_out::set_attr(unsigned char attr)
{
    m_attr = attr;
    SetConsoleTextAttribute(m_stdout, attr);
}



//------------------------------------------------------------------------------
void win_terminal::begin()
{
    win_terminal_in::begin();
    win_terminal_out::begin();
}

//------------------------------------------------------------------------------
void win_terminal::end()
{
    win_terminal_out::end();
    win_terminal_in::end();
}

//------------------------------------------------------------------------------
void win_terminal::select()
{
    win_terminal_in::select();
}

//------------------------------------------------------------------------------
int win_terminal::read()
{
    return win_terminal_in::read();
}

//------------------------------------------------------------------------------
void win_terminal::flush()
{
    win_terminal_out::flush();
}

//------------------------------------------------------------------------------
int win_terminal::get_columns() const
{
    return win_terminal_out::get_columns();
}

//------------------------------------------------------------------------------
int win_terminal::get_rows() const
{
    return win_terminal_out::get_rows();
}

//------------------------------------------------------------------------------
void win_terminal::write_c1(const ecma48_code& code)
{
    if (!m_enable_c1)
    {
        win_terminal_out::write(code.get_pointer(), code.get_length()); 
        return;
    }

    if (code.get_code() != ecma48_code::c1_csi)
        return;

    int final, params[32], param_count;
    param_count = code.decode_csi(final, params, sizeof_array(params));
    const array<int> params_array(params, param_count);

    switch (final)
    {
    case 'm':
        write_sgr(params_array);
        break;
    }
}

//------------------------------------------------------------------------------
void win_terminal::write_c0(int c0)
{
    switch (c0)
    {
    case 0x07:
        // MODE4
        break;

    default:
        {
            wchar_t c = wchar_t(c0);
            win_terminal_out::write(&c, 1);
        }
    }
}

//------------------------------------------------------------------------------
void win_terminal::write(const char* chars, int length)
{
    ecma48_iter iter(chars, m_state, length);
    while (const ecma48_code* code = iter.next())
    {
        switch (code->get_type())
        {
        case ecma48_code::type_chars:
            win_terminal_out::write(code->get_pointer(), code->get_length());
            break;

        case ecma48_code::type_c0:
            write_c0(code->get_code());
            break;

        case ecma48_code::type_c1:
            write_c1(*code);
            break;
        }
    }
}

//------------------------------------------------------------------------------
void win_terminal::check_c1_support()
{
    // Check for the presence of known third party tools that also provide ANSI
    // escape code support (MODE4)
    const char* dll_names[] = {
        "conemuhk.dll",
        "conemuhk64.dll",
        "ansi.dll",
        "ansi32.dll",
        "ansi64.dll",
    };

    for (int i = 0; i < sizeof_array(dll_names); ++i)
    {
        const char* dll_name = dll_names[i];
        if (GetModuleHandle(dll_name) != nullptr)
        {
            LOG("Disabling ANSI support. Found '%s'", dll_name);
            m_enable_c1 = false;
            return;
        }
    }

    // Give the user the option to disable ANSI support.
    m_enable_c1 = !g_ansi.get();
}

//------------------------------------------------------------------------------
void win_terminal::write_sgr(const array<int>& params)
{
    static const unsigned char sgr_to_attr[] = { 0, 4, 2, 6, 1, 5, 3, 7 };

    // Process each code that is supported.
    unsigned char attr = get_attr();
    for (int param : params)
    {
        if (param == 0) // reset
        {
            attr = get_default_attr();
        }
        else if (param == 1) // fg intensity (bright)
        {
            attr |= 0x08;
        }
        else if (param == 2 || param == 22) // fg intensity (normal)
        {
            attr &= ~0x08;
        }
        else if (param == 4) // bg intensity (bright)
        {
            attr |= 0x80;
        }
        else if (param == 24) // bg intensity (normal)
        {
            attr &= ~0x80;
        }
        else if (param - 30 < 8) // fg colour
        {
            attr = (attr & 0xf8) | sgr_to_attr[(param - 30) & 7];
        }
        else if (param - 90 < 8) // fg colour
        {
            attr |= 0x08;
            attr = (attr & 0xf8) | sgr_to_attr[(param - 90) & 7];
        }
        else if (param == 39) // default fg colour
        {
            attr = (attr & 0xf8) | (get_default_attr() & 0x07);
        }
        else if (param - 40 < 8) // bg colour
        {
            attr = (attr & 0x8f) | (sgr_to_attr[(param - 40) & 7] << 4);
        }
        else if (param - 100 < 8) // bg colour
        {
            attr |= 0x80;
            attr = (attr & 0x8f) | (sgr_to_attr[(param - 100) & 7] << 4);
        }
        else if (param == 49) // default bg colour
        {
            attr = (attr & 0x8f) | (get_default_attr() & 0x70);
        }
        else if (param == 38 || param == 48) // extended colour (skipped)
        {
            /* MODE4
            // format = param;5;[0-255] or param;2;r;g;b
            ++i;
            if (i >= csi.param_count)
                break;

            switch (csi.params[i])
            {
            case 2: i += 3; break;
            case 5: i += 1; break;
            }
            */

            continue;
        }
    }

    set_attr(attr);
}
