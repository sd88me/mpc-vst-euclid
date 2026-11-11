// Stand-in for the RtMidi library in tests/core_equiv.cpp: the original lane class (src/eqseq.cpp) sends into a list.
#pragma once
#include <vector>
struct RtMidiOut {
    std::vector<std::vector<unsigned char> > sent;
    void sendMessage(std::vector<unsigned char> *m) { sent.push_back(*m); }
};
