#include <cassert>
#include <iostream>
#include "../cam.ino"

static void input(const std::string& line) { Serial.input += line; readCommands(); }
static void finishFrame() {
    while (pending && !awaitingDone) input("ACK " + std::to_string(sequence) + " " + std::to_string(offset + sentLength) + "\n");
    assert(pending && awaitingDone);
    input("DONE " + std::to_string(sequence) + "\n");
    assert(!pending);
}
int main() {
    setup();
    assert(!streaming && !cameraReady && side==51 && colours==64);
    input("SIZE 72\n");
    input("STRE"); assert(!streaming);
    input("AM\r\n"); assert(streaming);
    clockMs = 40; loop();
    assert(pending && !previous.size() && encoded.mode != 'D');
    assert(frameGets == returns && frameGets == 2); // camera buffer already returned
    auto firstPacket = Serial.output.substr(Serial.output.find("NC2 BEGIN"));
    assert(firstPacket.size() < 400);
    Serial.output.clear();
    input("ACK 999999 192\n"); assert(offset == 0);
    streaming=false; snapshot=true;
    clockMs += AckTimeoutMs; loop();
    assert(retries == 1 && Serial.output == firstPacket); // lost ACK
    finishFrame();
    streaming=true;
    assert(previous.size() == 72 * 72 && previousSequence == sequence);
    input("SIZE 102\n"); assert(previous.empty() && side == 102);
    clockMs += 40; loop();
    assert(pending && encoded.mode != 'D');
    finishFrame();
    // An image with spatial structure makes the next unchanged frame a delta.
    for (size_t i = 0; i < sizeof(cameraData); ++i) cameraData[i] = (i / 24) % 256;
    clockMs += 40; loop(); finishFrame();
    clockMs += 40; loop(); assert(encoded.mode == 'D');
    const auto seq = sequence;
    input("NAK " + std::to_string(seq) + "\n");
    assert(!pending && previous.empty());
    clockMs += 40; loop(); assert(encoded.mode != 'D');
    input("STOP\n");
    assert(!pending && !streaming && !cameraReady && deinitializes == 1);
    input(std::string(80, 'S') + "\nSTREAM\n"); assert(streaming);
    clockMs += 40; loop();
    streaming=false; snapshot=true;
    for (unsigned n = 0; n <= MaxRetries; ++n) { clockMs += AckTimeoutMs; loop(); }
    assert(!pending && previous.empty() && !streaming && !cameraReady);
    assert(Serial.output.find("NC2 ERROR ack-timeout") != std::string::npos);
    input("SIZE 999\n"); assert(side == 102);
    input("PIC\n"); clockMs += 40; loop(); finishFrame();
    assert(!snapshot && !streaming && !pending);
    input("PIC\n"); failCapture = true; clockMs += 40; loop();
    assert(!snapshot && !pending && !cameraReady);
    assert(frameGets == returns);
    // uint32 millisecond rollover does not disable transfer retries.
    failCapture = false; input("PIC\n"); clockMs = UINT32_MAX - 10; loop();
    assert(pending); clockMs += AckTimeoutMs; loop(); assert(retries == 1);
    input("STOP\nWINDOW 2\nFORMAT 64\nNOISE 8\nSIZE 51\nSTREAM\n");
    clockMs+=40; loop();
    assert(pending && sentLength==384);
    const auto startOffset=offset;
    input("ACK "+std::to_string(sequence)+" 192\n"); assert(offset==startOffset);
    clockMs+=181; loop();
    assert(!pending && streaming && previous.empty() && droppedFrames==1);
    assert(Serial.output.find("NC2 DROP ")!=std::string::npos);
    clockMs+=40; loop(); assert(encoded.mode!='D'); finishFrame();
    input("METRICS 1\nFORMAT 16\nPIC\n");
    clockMs+=40; loop(); finishFrame();
    for (auto v:previous) assert(v<=15);
    assert(Serial.output.find("NC2 STATS ")!=std::string::npos);
    input("STREAM\n"); clockMs+=40; loop();
    // Total frame deadline also expires when individual ACKs arrive on time.
    frameStarted=clockMs-liveBudget;
    loop(); assert(!pending && droppedFrames==2);
    for(unsigned n=0;n<7;++n) { clockMs+=40; loop(); clockMs+=181; loop(); }
    assert(!streaming && !cameraReady);
    std::cout << "Firmware tests passed: command fragmentation, ACK loss, delta base, STOP, timeout, snapshots and clock rollover\n";
}
