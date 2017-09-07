#include "Via.h"
#include "MemoryMap.h"

void Via::Init(MemoryBus& memoryBus) {
    memoryBus.ConnectDevice(*this, MemoryMap::Via.range);
    B = A = 0;
    DataDirB = DataDirA = 0;
    Timer1Low = Timer1High = 0;
    Timer1LatchLow = Timer1LatchHigh = 0;
    Timer2Low = Timer2High = 0;
    Shift = 0;
    AuxCntl = 0;
    PeriphCntl = 0;
    InterruptFlag = 0;
    InterruptEnable = 0;
}

void Via::Update(cycles_t cycles) {
    m_timer1.Update(cycles);

    // Update timer 1 interrupt flag (bit 6)
    InterruptFlag |= (m_timer1.InterruptEnabled() ? (1 << 6) : 0);

    // Integrators are enabled while RAMP line is active (low)
    // bool integratorEnabled = (B & 0b1000'0000) == 0; // /RAMP

    // if (integratorEnabled) {
    //    Vector2 integratorInput;
    //    integratorInput.x = (m_velocity.x - 128.f) * (10.f / 256.f);
    //    integratorInput.y = (m_velocity.y - 128.f) * (10.f / 256.f);

    //    float integratorOffset = (m_xyOffset - 128.f) * (10.f / 256.f);

    //    Vector2 targetPos;
    //    targetPos.x =
    //        -((10000.0f * (integratorInput.x - integratorOffset) * (float)deltaTime) + m_pos.x);
    //    targetPos.y =
    //        -((10000.0f * (integratorInput.y - integratorOffset) * (float)deltaTime) + m_pos.y);

    //    bool drawEnabled = !m_blank && m_brightness > 0.f;

    //    if (drawEnabled) {
    //        // printf("Added line!\n");
    //        m_lines.emplace_back(Line{m_pos, targetPos});
    //    }

    //    m_pos = targetPos;
    //}
}

uint8_t Via::Read(uint16_t address) const {
    const uint16_t index = MemoryMap::Via.MapAddress(address);
    switch (index) {
    case 0:
        return B;
    case 1:
        return A;
    case 2:
        return DataDirB;
    case 3:
        return DataDirA;
    case 4:
        return Timer1Low;
    case 5:
        return Timer1High;
    case 6:
        FAIL_MSG("Not implemented. Not sure we need this.");
        return Timer1LatchLow;
    case 7:
        FAIL_MSG("Not implemented. Not sure we need this.");
        return Timer1LatchHigh;
    case 8:
        FAIL_MSG("Not implemented. Not sure we need this.");
        // return Timer2Low;
        return 0;
    case 9:
        FAIL_MSG("Not implemented. Not sure we need this. 2");
        // return Timer2High;
        return 0;
    case 10:
        return Shift;
    case 11:
        return AuxCntl;
    case 12:
        return PeriphCntl;
    case 13:
        return InterruptFlag;
    case 14:
        FAIL_MSG("Not implemented");
        return InterruptEnable;
    case 15:
        FAIL_MSG("A without handshake not implemented yet");
        break;
    }
    return 0;
}

void Via::Write(uint16_t address, uint8_t value) {
    const uint16_t index = MemoryMap::Via.MapAddress(address);
    switch (index) {
    case 0:
        B = value;
        goto UPDATE_INTEGRATORS;
        break;
    case 1: {
        // Port A is connected directly to the DAC, which in turn is connected to both a MUX with 4
        // outputs, and to the X-axis integrator.
        A = value;

    UPDATE_INTEGRATORS:

        // Port B bit 0 is connected to the MUX enable signal (when low)
        if ((B & 0b0000'0001) == 0) {
            // MUX enabled
            switch ((B & 0b0000'0110) >> 1) {
            case 0:
                // Y-axis integrator
                // printf("Writing Y-axis integrator value!\n");
                m_velocity.y = A;
                break;

            case 1:
                // X,Y Axis integrator offset
                m_xyOffset = A;
                break;

            case 2:
                // Z Axis (Vector Brightness) level
                m_brightness = A;
                break;

            case 3:
                // Connected to sound output line via divider network
                //@TODO
                break;

            default:
                FAIL();
                break;
            }
        } else {
            // printf("Writing X-axis integrator value!\n");
            // MUX disabled so we output to X-axis integrator
            m_velocity.x = A;
        }
    } break;
    case 2:
        DataDirB = value;
        break;
    case 3:
        DataDirA = value;
        break;
    case 4:
        Timer1Low = value;
        break;
    case 5:
        Timer1High = value;
        break;
    case 6:
        Timer1LatchLow = value;
        break;
    case 7:
        Timer1LatchHigh = value;
        break;
    case 8:
        // Timer2Low = value;
        m_timer1.SetCounterLow(value);
        break;
    case 9:
        // Timer2High = value;
        m_timer1.SetCounterHigh(value);
        break;
    case 10:
        Shift = value;
        break;
    case 11:
        ASSERT_MSG((value & 0b0110'0000) == 0, "t1 and t2 assumed to be always in one-shot mode");
        AuxCntl = value;
        break;
    case 12: {
        PeriphCntl = value;

        // CA2 is used to control /ZERO which is used bring beam to center of screen (0,0)
        uint8_t CA2 = (PeriphCntl & 0b0000'1110) >> 1;
        switch (CA2) {
        case 0b110:
            m_pos = {0.f, 0.f};
            m_lines.clear(); // HACK!
            break;
        case 0b111:
            // Nothing to do
            break;
        default:
            FAIL_MSG("Top 2 bits should be 1 according to docs (right?)");
            break;
        }

        // CB2 is used to control /BLANK which is used to enable/disable beam (drawing)
        uint8_t CB2 = (PeriphCntl & 0b1110'0000) >> 5;
        switch (CB2) {
        case 0b110:
            // m_blank = true;
            break;
        case 0b111:
            m_blank = false;
            break;
        default:
            FAIL_MSG("Top 2 bits should be 1 according to docs (right?)");
            break;
        }

    } break;
    case 13:
        FAIL_MSG("Not implemented");
        InterruptFlag = value;
        break;
    case 14:
        FAIL_MSG("Not implemented");
        InterruptEnable = value;
        break;
    case 15:
        FAIL_MSG("A without handshake not implemented yet");
        break;
    }
}
