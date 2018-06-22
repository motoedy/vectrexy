#include "Psg.h"
#include "BitOps.h"

namespace {
    namespace Register {
        enum Type {
            ChannelALow = 0,
            ChannelAHigh = 1,
            ChannelBLow = 2,
            ChannelBHigh = 3,
            ChannelCLow = 4,
            ChannelCHigh = 5,

            MixerControl = 7,

            AmplitudeA = 10,
            AmplitudeB = 11,
            AmplitudeC = 12,
        };
    }

    namespace MixerControl {
        const uint8_t ToneA = BITS(0);
        const uint8_t ToneB = BITS(1);
        const uint8_t ToneC = BITS(2);
        const uint8_t NoiseA = BITS(3);
        const uint8_t NoiseB = BITS(4);
        const uint8_t NoiseC = BITS(5);
        // Bits 6 and 7 are to control IO ports A and B, but we don't use this on Vectrex

        bool IsEnabled(uint8_t reg, uint8_t type) {
            // Enabled when bit is 0
            return !TestBits(reg, type);
        }

        uint8_t ToneChannelByIndex(int index) { return ToneA << index; }
        uint8_t NoiseChannelByIndex(int index) { return NoiseA << index; }

    } // namespace MixerControl

    namespace AmplitudeControl {
        const uint8_t FixedVolume = BITS(0, 1, 2, 3);
        const uint8_t EnvelopeMode = BITS(4);
        const uint8_t Unused = BITS(5, 6, 7);

        enum class Mode { Fixed, Envelope };
        Mode GetMode(uint8_t reg) {
            return TestBits(reg, EnvelopeMode) ? Mode::Envelope : Mode::Fixed;
        }

        float GetFixedVolumeRatio(uint8_t reg) {
            assert(GetMode(reg) == Mode::Fixed);
            return ReadBits(reg, FixedVolume) / 16.f;
        }

    } // namespace AmplitudeControl
} // namespace

void Psg::Init() {
    Reset();
}

void Psg::WriteDA(uint8_t value) {
    m_DA = value;
}

uint8_t Psg::ReadDA() {
    return m_DA;
}

void Psg::Reset() {
    m_mode = {};
    m_DA = {};
    m_registers.fill(0);
    m_masterDivider.Reset();
    m_toneGenerators = {};
}

void Psg::Update(cycles_t cycles) {
    for (cycles_t cycle = 0; cycle < cycles; ++cycle) {
        Clock();
    }
}

void Psg::Clock() {
    auto ModeFromBDIRandBC1 = [](bool BDIR, bool BC1) -> Psg::PsgMode {
        uint8_t value{};
        SetBits(value, 1, BDIR);
        SetBits(value, 0, BC1);
        return static_cast<Psg::PsgMode>(value);
    };

    const auto lastMode = m_mode;
    m_mode = ModeFromBDIRandBC1(m_BDIR, m_BC1);

    switch (m_mode) {
    case PsgMode::Inactive:
        break;
    case PsgMode::Read:
        if (lastMode == PsgMode::Inactive) {
            m_DA = Read(m_latchedAddress);
        }
        break;
    case PsgMode::Write:
        if (lastMode == PsgMode::Inactive) {
            Write(m_latchedAddress, m_DA);
        }
        break;
    case PsgMode::LatchAddress:
        if (lastMode == PsgMode::Inactive) {
            m_latchedAddress = ReadBits(m_DA, 0b1111);
        }
        break;
    }

    // Clock generators every 16 input clocks
    if (m_masterDivider.Clock()) {
        for (auto& swg : m_toneGenerators) {
            swg.Clock();
        }
    }

    auto GetChannelVolume = [](uint8_t& amplitudeRegister) {
        if (AmplitudeControl::GetMode(amplitudeRegister) == AmplitudeControl::Mode::Fixed) {
            return AmplitudeControl::GetFixedVolumeRatio(amplitudeRegister);
        } else {
            //@TODO: envelope volume...
            return 0.f;
        }
    };

    auto SampleChannel = [&GetChannelVolume](uint8_t& amplitudeRegister,
                                             uint8_t& mixerControlRegister, int index,
                                             ToneGenerator& toneGenerator) -> float {
        auto toneChannel = MixerControl::ToneChannelByIndex(index);
        auto noiseChannel = MixerControl::NoiseChannelByIndex(index);

        const float volume = GetChannelVolume(amplitudeRegister);

        if (volume == 0.f)
            return 0.f;

        float sample = 0;
        float numValues = 0;
        if (MixerControl::IsEnabled(mixerControlRegister, toneChannel)) {
            sample += toneGenerator.Value();
            ++numValues;
        }

        if (MixerControl::IsEnabled(mixerControlRegister, noiseChannel)) {
            //@TODO:
            // sample += m_noiseGenerator.Value();
            //++numValues;
        }

        return sample * volume;
    };

    auto SampleAllChannels = [this, &SampleChannel] {
        float sample = 0.f;
        for (int i = 0; i < 3; ++i) {
            auto& amplitudeRegister = m_registers[Register::AmplitudeA + i];
            auto& mixerControlRegister = m_registers[Register::MixerControl];
            sample +=
                SampleChannel(amplitudeRegister, mixerControlRegister, i, m_toneGenerators[i]);
        }
        sample /= 6;
        return sample;
    };
}

uint8_t Psg::Read(uint16_t address) {
    switch (m_latchedAddress) {
    case Register::ChannelAHigh:
        return m_toneGenerators[0].PeriodHigh();
    case Register::ChannelALow:
        return m_toneGenerators[0].PeriodLow();
    case Register::ChannelBHigh:
        return m_toneGenerators[1].PeriodHigh();
    case Register::ChannelBLow:
        return m_toneGenerators[1].PeriodLow();
    case Register::ChannelCHigh:
        return m_toneGenerators[2].PeriodHigh();
    case Register::ChannelCLow:
        return m_toneGenerators[2].PeriodLow();
    }

    return m_registers[address];
}

void Psg::Write(uint16_t address, uint8_t value) {
    switch (m_latchedAddress) {
    case Register::ChannelAHigh:
        return m_toneGenerators[0].SetPeriodHigh(value);
    case Register::ChannelALow:
        return m_toneGenerators[0].SetPeriodLow(value);
    case Register::ChannelBHigh:
        return m_toneGenerators[1].SetPeriodHigh(value);
    case Register::ChannelBLow:
        return m_toneGenerators[1].SetPeriodLow(value);
    case Register::ChannelCHigh:
        return m_toneGenerators[2].SetPeriodHigh(value);
    case Register::ChannelCLow:
        return m_toneGenerators[2].SetPeriodLow(value);
    case Register::MixerControl:
        ASSERT_MSG(ReadBits(value, 0b1100'0000) == 0, "Not supporting I/O ports on PSG");
        break;
    }

    m_registers[address] = value;
}
