#pragma once
// Private engine helper shared by the platform line readers (hosted and bare
// metal). Freestanding: no includes.
//
// Drops terminal escape sequences from typed input -- arrow keys, Home/End,
// Delete, F-keys (ESC [ <params> <final>, ESC O <final>) -- so they never end
// up inside a command line as "[A" or "[D". A lone ESC (the Esc key) is
// dropped and the character after it is kept.

namespace osef::detail {

class EscapeFilter {
public:
    // True if `ch` belongs to an escape sequence and must be ignored.
    bool swallow(char ch) {
        const auto u = static_cast<unsigned char>(ch);
        switch (state_) {
        case State::Text:
            if (u != 0x1b) return false;
            state_ = State::Escape;
            return true;
        case State::Escape:
            if (ch == '[' || ch == 'O') {
                state_ = State::Sequence;
                return true;
            }
            state_ = State::Text;
            return swallow(ch);
        case State::Sequence:
            // Enter inside a sequence: it was not one after all; keep the line.
            if (ch == '\n' || ch == '\r') {
                state_ = State::Text;
                return false;
            }
            if (u >= 0x40 && u <= 0x7e) state_ = State::Text;  // final byte
            return true;                                        // parameter / final bytes
        }
        return false;
    }

private:
    enum class State { Text, Escape, Sequence };
    State state_ = State::Text;
};

} // namespace osef::detail
