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
    assert(!streaming && !cameraReady);
    input("STRE"); assert(!streaming);
    input("AM\r\n"); assert(streaming);
    clockMs = 40; loop();
    assert(pending && !previous.size() && encoded.mode != 'D');
    assert(frameGets == returns && frameGets == 2); // camera buffer already returned
    auto firstPacket = Serial.output.substr(Serial.output.find("NC2 BEGIN"));
    assert(firstPacket.size() < 400);
    Serial.output.clear();
    input("ACK 999999 192\n"); assert(offset == 0);
    clockMs += AckTimeoutMs; loop();
    assert(retries == 1 && Serial.output == firstPacket); // lost ACK
    finishFrame();
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
    failCapture = false; input("STREAM\n"); clockMs = UINT32_MAX - 10; loop();
    assert(pending); clockMs += AckTimeoutMs; loop(); assert(retries == 1);
    std::cout << "Firmware tests passed: command fragmentation, ACK loss, delta base, STOP, timeout, snapshots and clock rollover\n";
}
