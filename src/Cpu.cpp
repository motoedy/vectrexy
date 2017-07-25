#include "Cpu.h"
#include "CpuHelpers.h"
#include "CpuOpCodes.h"
#include "MemoryBus.h"
#include <array>
#include <type_traits>

namespace {
    template <typename T>
    size_t NumBitsSet(T value) {
        size_t count = 0;
        while (value) {
            if ((value & 0x1) != 0)
                ++count;
            value >>= 1;
        }
        return count;
    }

    template <typename T>
    uint8_t CalcZero(T v) {
        return v == 0;
    }

    uint8_t CalcNegative(uint8_t v) { return (v & BITS(7)) != 0; }
    uint8_t CalcNegative(uint16_t v) { return (v & BITS(15)) != 0; }

    uint8_t CalcCarry(uint16_t r) { return (r & 0xFF00) != 0; }
    uint8_t CalcCarry(uint32_t r) { return (r & 0xFFFF'0000) != 0; }
    uint8_t CalcCarry(uint8_t r) = delete; // Result must be larger than 8 or 16 bits

    uint8_t CalcHalfCarryFromAdd(uint8_t a, uint8_t b) {
        return (((a & 0x0F) + (b & 0x0F)) & 0xF0) != 0;
    }
    uint8_t CalcHalfCarryFromAdd(uint16_t a,
                                 uint16_t b) = delete; // Only 8-bit add computes half-carry

    uint8_t CalcOverflow(uint8_t a, uint8_t b, uint16_t r) {
        // Given r = a + b, overflow occurs if both a and b are negative and r is positive, or both
        // a and b are positive and r is negative. Looking at sign bits of a, b, and r, overflow
        // occurs when 0 0 1 or 1 1 0.
        return (((uint16_t)a ^ r) & ((uint16_t)b ^ r) & BITS(7)) != 0;
    }
    uint8_t CalcOverflow(uint16_t a, uint16_t b, uint32_t r) {
        return (((uint32_t)a ^ r) & ((uint32_t)b ^ r) & BITS(15)) != 0;
    }
    template <typename T>
    uint8_t CalcOverflow(T a, T b, uint8_t r) = delete; // Result must be larger than 8 or 16 bits

} // namespace

class CpuImpl : public CpuRegisters {
public:
    MemoryBus* m_memoryBus = nullptr;
    cycles_t m_cycles = 0;

    void Init(MemoryBus& memoryBus) {
        m_memoryBus = &memoryBus;
        Reset(0);
    }

    void Reset(uint16_t initialPC) {
        X = 0;
        Y = 0;
        U = 0;
        S = 0; // BIOS will init this to 0xCBEA, which is the last byte of programmer-usable RAM
        PC = initialPC;
        DP = 0;

        CC.Value = 0;
        CC.InterruptMask = true;
        CC.FastInterruptMask = true;
    }

    uint8_t Read8(uint16_t address) { return m_memoryBus->Read(address); }

    uint16_t Read16(uint16_t address) {
        // Big endian
        auto high = m_memoryBus->Read(address++);
        auto low = m_memoryBus->Read(address);
        return CombineToU16(high, low);
    }

    uint8_t ReadPC8() { return Read8(PC++); }
    uint16_t ReadPC16() {
        uint16_t value = Read16(PC);
        PC += 2;
        return value;
    }

    void Push8(uint16_t& stackPointer, uint8_t value) { m_memoryBus->Write(--stackPointer, value); }

    uint8_t Pop8(uint16_t& stackPointer) {
        auto value = m_memoryBus->Read(stackPointer++);
        return value;
    }

    void Push16(uint16_t& stackPointer, uint16_t value) {
        m_memoryBus->Write(--stackPointer, U8(value & 0xFF)); // Low
        m_memoryBus->Write(--stackPointer, U8(value >> 8));   // High
    }

    uint16_t Pop16(uint16_t& stackPointer) {
        auto high = m_memoryBus->Read(stackPointer++);
        auto low = m_memoryBus->Read(stackPointer++);
        return CombineToU16(high, low);
    }

    uint16_t ReadDirectEA() {
        // EA = DP : (PC)
        uint16_t EA = CombineToU16(DP, ReadPC8());
        return EA;
    }

    uint16_t ReadIndexedEA() {
        // In all indexed addressing one of the pointer registers (X, Y, U, S and sometimes PC) is
        // used in a calculation of the EA. The postbyte specifies type and variation of addressing
        // mode as well as pointer registers to be used.
        //@TODO: add extra cycles

        auto RegisterSelect = [this](uint8_t postbyte) -> uint16_t& {
            switch ((postbyte >> 5) & 0b11) {
            case 0b00:
                return X;
            case 0b01:
                return Y;
            case 0b10:
                return U;
            default: // 0b11:
                return S;
            }
        };

        uint16_t EA = 0;
        uint8_t postbyte = ReadPC8();
        bool supportsIndirect = true;
        int extraCycles = 0;

        if ((postbyte & BITS(7)) == 0) // (+/- 4 bit offset),R
        {
            // postbyte is a 5 bit two's complement number we convert to 8 bit.
            // So if bit 4 is set (sign bit), we extend the sign bit by turning on bits 6,7,8;
            uint8_t offset = postbyte & 0b0000'1111;
            if (postbyte & BITS(4))
                offset |= 0b1110'0000;
            EA = RegisterSelect(postbyte) + S16(offset);
            supportsIndirect = false;
            extraCycles = 1;
        } else {
            switch (postbyte & 0b1111) {
            case 0b0000: { // ,R+
                auto& reg = RegisterSelect(postbyte);
                EA = reg;
                reg += 1;
                supportsIndirect = false;
                extraCycles = 2;
            } break;
            case 0b0001: { // ,R++
                auto& reg = RegisterSelect(postbyte);
                EA = reg;
                reg += 2;
                extraCycles = 3;
            } break;
            case 0b0010: { // ,-R
                auto& reg = RegisterSelect(postbyte);
                reg -= 1;
                EA = reg;
                supportsIndirect = false;
                extraCycles = 2;
            } break;
            case 0b0011: { // ,--R
                auto& reg = RegisterSelect(postbyte);
                reg -= 2;
                EA = reg;
                extraCycles = 3;
            } break;
            case 0b0100: // ,R
                EA = RegisterSelect(postbyte);
                break;
            case 0b0101: // (+/- B),R
                EA = RegisterSelect(postbyte) + S16(B);
                extraCycles = 1;
                break;
            case 0b0110: // (+/- A),R
                EA = RegisterSelect(postbyte) + S16(A);
                extraCycles = 1;
                break;
            case 0b0111:
                FAIL("Illegal");
                break;
            case 0b1000: { // (+/- 7 bit offset),R
                uint8_t postbyte2 = ReadPC8();
                EA = RegisterSelect(postbyte) + S16(postbyte2);
                extraCycles = 1;
            } break;
            case 0b1001: { // (+/- 15 bit offset),R
                uint8_t postbyte2 = ReadPC8();
                uint8_t postbyte3 = ReadPC8();
                EA = RegisterSelect(postbyte) + CombineToS16(postbyte2, postbyte3);
                extraCycles = 4;
            } break;
            case 0b1010:
                FAIL("Illegal");
                break;
            case 0b1011: // (+/- D),R
                EA = RegisterSelect(postbyte) + S16(D);
                extraCycles = 4;
                break;
            case 0b1100: { // (+/- 7 bit offset),PC
                uint8_t postbyte2 = ReadPC8();
                EA = PC + S16(postbyte2);
                extraCycles = 1;
            } break;
            case 0b1101: { // (+/- 15 bit offset),PC
                uint8_t postbyte2 = ReadPC8();
                uint8_t postbyte3 = ReadPC8();
                EA = PC + CombineToS16(postbyte2, postbyte3);
                extraCycles = 5;
            } break;
            case 0b1110:
                FAIL("Illegal");
                break;
            case 0b1111: { // [address] (Indirect-only)
                uint8_t postbyte2 = ReadPC8();
                uint8_t postbyte3 = ReadPC8();
                EA = CombineToS16(postbyte2, postbyte3);
                extraCycles = 5;
            } break;
            default:
                FAIL("Illegal");
                break;
            }
        }

        if (supportsIndirect && (postbyte & BITS(4))) {
            uint8_t msb = m_memoryBus->Read(EA);
            uint8_t lsb = m_memoryBus->Read(EA + 1);
            EA = CombineToU16(msb, lsb);
            extraCycles += 3;
        }

        m_cycles += extraCycles;

        return EA;
    }

    uint16_t ReadExtendedEA() {
        // Contents of 2 bytes following opcode byte specify 16-bit effective address (always 3 byte
        // instruction) EA = (PC) : (PC + 1)
        auto msb = ReadPC8();
        auto lsb = ReadPC8();
        uint16_t EA = CombineToU16(msb, lsb);

        // @TODO: "As a special case of indexed addressing, one level of indirection may be added to
        // extended addressing. In extended indirect,
        //         the two bytes following the postbyte of an indexed instruction contain the
        //         address of the data."
        // *** Is this handled under Indexed??

        return EA;
    }

    // Read 16-bit effective address based on addressing mode
    template <AddressingMode addressingMode>
    uint16_t ReadEA16() {
        assert(false && "Not implemented for addressing mode");
        return 0xFFFF;
    }
    template <>
    uint16_t ReadEA16<AddressingMode::Indexed>() {
        return ReadIndexedEA();
    }
    template <>
    uint16_t ReadEA16<AddressingMode::Extended>() {
        return ReadExtendedEA();
    }
    template <>
    uint16_t ReadEA16<AddressingMode::Direct>() {
        return ReadDirectEA();
    }

    // Read CPU op's value (8/16 bit) either directly or indirectly (via EA) depending on addressing
    // mode Default template assumes operand is EA and de-refs it
    template <AddressingMode addressingMode>
    uint16_t ReadOperandValue16() {
        auto EA = ReadEA16<addressingMode>();
        return Read16(EA);
    }
    // Specialize for Immediate mode where we don't de-ref
    template <>
    uint16_t ReadOperandValue16<AddressingMode::Immediate>() {
        return ReadPC16();
    }

    template <AddressingMode addressingMode>
    uint8_t ReadOperandValue8() {
        auto EA = ReadEA16<addressingMode>();
        return Read8(EA);
    }
    template <>
    uint8_t ReadOperandValue8<AddressingMode::Immediate>() {
        return ReadPC8();
    }

    // Read CPU op's relative offset from next 8/16 bits
    int8_t ReadRelativeOffset8() { return static_cast<int8_t>(ReadPC8()); }
    int16_t ReadRelativeOffset16() { return static_cast<int16_t>(ReadPC16()); }

    template <int page, uint8_t opCode>
    void OpLD(uint8_t& targetReg) {
        uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        CC.Negative = (value & BITS(7)) != 0;
        CC.Zero = (value == 0);
        CC.Overflow = 0;
        targetReg = value;
    }

    template <int page, uint8_t opCode>
    void OpLD(uint16_t& targetReg) {
        uint16_t value = ReadOperandValue16<LookupCpuOp(page, opCode).addrMode>();
        CC.Negative = (value & BITS(15)) != 0;
        CC.Zero = (value == 0);
        CC.Overflow = 0;
        targetReg = value;
    }

    template <int page, uint8_t opCode>
    void OpST(const uint8_t& sourceReg) {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        m_memoryBus->Write(EA, sourceReg);
        CC.Negative = (sourceReg & BITS(7)) != 0;
        CC.Zero = (sourceReg == 0);
        CC.Overflow = 0;
    }

    template <int page, uint8_t opCode>
    void OpST(const uint16_t& sourceReg) {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        m_memoryBus->Write(EA, U8(sourceReg >> 8));       // High
        m_memoryBus->Write(EA + 1, U8(sourceReg & 0xFF)); // Low
        CC.Negative = (sourceReg & BITS(7)) != 0;
        CC.Zero = (sourceReg == 0);
        CC.Overflow = 0;
    }

    template <int page, uint8_t opCode>
    void OpLEA(uint16_t& reg) {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        reg = EA;
        // Zero flag not affected by LEAU/LEAS
        if (&reg == &X || &reg == &Y) {
            CC.Zero = (reg == 0);
        }
    }

    template <int page, uint8_t opCode>
    void OpJSR() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        Push16(S, PC);
        PC = EA;
    }

    template <int page, uint8_t opCode>
    void OpCLR() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        m_memoryBus->Write(EA, 0);
        CC.Negative = 0;
        CC.Zero = 1;
        CC.Overflow = 0;
        CC.Carry = 0;
    }

    void OpCLR(uint8_t& reg) {
        reg = 0;
        CC.Negative = 0;
        CC.Zero = 1;
        CC.Overflow = 0;
        CC.Carry = 0;
    }

    enum class UpdateHalfCarry { False, True };

    static uint8_t AddImpl(uint8_t a, uint8_t b, uint8_t carry, ConditionCode& CC,
                           UpdateHalfCarry updateHalfCarry) {
        uint16_t r16 = a + b + carry;
        if (updateHalfCarry == UpdateHalfCarry::True) {
            CC.HalfCarry =
                CalcHalfCarryFromAdd(a, b); //@TODO: Should ONLY be computed for ADDA/ADDB
        }
        CC.Carry = CalcCarry(r16);
        CC.Overflow = CalcOverflow(a, b, r16);
        uint8_t r = static_cast<uint8_t>(r16);
        CC.Zero = CalcZero(r);
        CC.Negative = CalcNegative(r);
        return r;
    }
    static uint16_t AddImpl(uint16_t a, uint16_t b, uint16_t carry, ConditionCode& CC,
                            UpdateHalfCarry updateHalfCarry) {
        (void)updateHalfCarry;
        assert(updateHalfCarry == UpdateHalfCarry::False); // 16-bit version never updates this

        uint32_t r32 = a + b + carry;
        // CC.HalfCarry = CalcHalfCarryFromAdd(a, b);
        CC.Carry = CalcCarry(r32);
        CC.Overflow = CalcOverflow(a, b, r32);
        uint16_t r = static_cast<uint16_t>(r32);
        CC.Zero = CalcZero(r);
        CC.Negative = CalcNegative(r);
        return r;
    }

    static uint8_t SubtractImpl(uint8_t a, uint8_t b, ConditionCode& CC) {
        auto result = AddImpl(a, ~b, 1, CC, UpdateHalfCarry::False);
        CC.Carry = !CC.Carry; // Carry is set if no borrow occurs
        return result;
    }
    static uint16_t SubtractImpl(uint16_t a, uint16_t b, ConditionCode& CC) {
        auto result = AddImpl(a, ~b, 1, CC, UpdateHalfCarry::False);
        CC.Carry = !CC.Carry; // Carry is set if no borrow occurs
        return result;
    }

    // ADDA, ADDB
    template <int page, uint8_t opCode>
    void OpADD(uint8_t& reg) {
        uint8_t b = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        reg = AddImpl(reg, b, 0, CC, UpdateHalfCarry::True);
    }

    // ADDD
    template <int page, uint8_t opCode>
    void OpADD(uint16_t& reg) {
        uint16_t b = ReadOperandValue16<LookupCpuOp(page, opCode).addrMode>();
        reg = AddImpl(reg, b, 0, CC, UpdateHalfCarry::False);
    }

    // SUBA, SUBB
    template <int page, uint8_t opCode>
    void OpSUB(uint8_t& reg) {
        reg = SubtractImpl(reg, ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>(), CC);
    }

    // SUBD
    template <int page, uint8_t opCode>
    void OpSUB(uint16_t& reg) {
        reg = SubtractImpl(reg, ReadOperandValue16<LookupCpuOp(page, opCode).addrMode>(), CC);
    }

    // ADCA, ADCB
    template <int page, uint8_t opCode>
    void OpADC(uint8_t& reg) {
        uint8_t b = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        reg = AddImpl(reg, b, CC.Carry, CC, UpdateHalfCarry::True);
    }

    // NEGA, NEGB
    template <int page, uint8_t opCode>
    void OpNEG(uint8_t& value) {
        auto origValue = value;
        value = -value;
        CC.Overflow = origValue == 0b1000'0000;
        CC.Zero = value == 0;
        CC.Negative = (value & BITS(7)) != 0;
        CC.Carry = origValue != 0; //@TODO: double check this
    }

    // NEG <address>
    template <int page, uint8_t opCode>
    void OpNEG() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        OpNEG<page, opCode>(value);
        m_memoryBus->Write(EA, value);
    }

    // INCA, INCB
    template <int page, uint8_t opCode>
    void OpINC(uint8_t& reg) {
        ++reg;
        CC.Overflow = reg == 0;
        CC.Zero = (reg == 0);
        CC.Negative = (reg & BITS(7)) != 0;
    }

    // INC <address>
    template <int page, uint8_t opCode>
    void OpINC() {
        //@TODO: refactor in terms of OpINC(uint8_t&) like OpNEG
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        ++value;
        m_memoryBus->Write(EA, value);
        CC.Overflow = value == 0;
        CC.Zero = (value == 0);
        CC.Negative = (value & BITS(7)) != 0;
    }

    // DECA, DECB
    template <int page, uint8_t opCode>
    void OpDEC(uint8_t& reg) {
        uint8_t origReg = reg;
        --reg;
        CC.Overflow = origReg == 0b1000'0000; // Could also set to (reg == 0b01111'1111)
        CC.Zero = (reg == 0);
        CC.Negative = (reg & BITS(7)) != 0;
    }

    // DEC <address>
    template <int page, uint8_t opCode>
    void OpDEC() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        uint8_t origValue = value;
        --value;
        m_memoryBus->Write(EA, value);
        CC.Overflow = origValue == 0b1000'0000;
        CC.Zero = (value == 0);
        CC.Negative = (value & BITS(7)) != 0;
    }

    template <int page, uint8_t opCode>
    void OpASR(uint8_t& value) {
        auto origValue = value;
        value = (origValue & 0b1000'0000) | (value >> 1);
        CC.Zero = (value == 0);
        CC.Negative = (value & BITS(7)) != 0;
        CC.Carry = origValue & 0b0000'0001;
    }

    template <int page, uint8_t opCode>
    void OpASR() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        OpASR<page, opCode>(value);
        m_memoryBus->Write(EA, value);
    }

    template <int page, uint8_t opCode>
    void OpLSR(uint8_t& value) {
        auto origValue = value;
        value = (value >> 1);
        CC.Zero = (value == 0);
        CC.Negative = 0; // Bit 7 always shifted out
        CC.Carry = origValue & 0b0000'0001;
    }

    template <int page, uint8_t opCode>
    void OpLSR() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        OpLSR<page, opCode>(value);
        m_memoryBus->Write(EA, value);
    }

    template <int page, uint8_t opCode>
    void OpCOM(uint8_t& value) {
        value = ~value;
        CC.Negative = CalcNegative(value);
        CC.Zero = CalcZero(value);
        CC.Overflow = 0;
        CC.Carry = 1;
    }

    template <int page, uint8_t opCode>
    void OpCOM() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        OpCOM<page, opCode>(value);
        m_memoryBus->Write(EA, value);
    }

    template <int page, uint8_t opCode>
    void OpASL(uint8_t& value) {
        auto origValue = value;
        value = (value << 1);
        CC.Zero = (value == 0);
        CC.Negative = (value & BITS(7)) != 0;
        CC.Carry = origValue & 0b1000'0000;
        // Overflow (sign change) happens if bit 7 or 6 was set, but not both
        CC.Overflow = (origValue >> 7) ^ (origValue >> 6);
    }

    template <int page, uint8_t opCode>
    void OpASL() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        uint8_t value = m_memoryBus->Read(EA);
        OpASL<page, opCode>(value);
        m_memoryBus->Write(EA, value);
    }

    template <int page, uint8_t opCode>
    void OpJMP() {
        uint16_t EA = ReadEA16<LookupCpuOp(page, opCode).addrMode>();
        PC = EA;
    }

    template <int page, uint8_t opCode>
    void OpPSH(uint16_t& stackReg) {
        assert(&stackReg == &S || &stackReg == &U);
        const uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        if (value & BITS(7))
            Push16(stackReg, PC);
        if (value & BITS(6)) {
            auto otherStackReg = &stackReg == &S ? U : S;
            Push16(stackReg, otherStackReg);
        }
        if (value & BITS(5))
            Push16(stackReg, Y);
        if (value & BITS(4))
            Push16(stackReg, X);
        if (value & BITS(3))
            Push8(stackReg, DP);
        if (value & BITS(2))
            Push8(stackReg, B);
        if (value & BITS(1))
            Push8(stackReg, A);
        if (value & BITS(0))
            Push8(stackReg, CC.Value);

        m_cycles += NumBitsSet(value); // 1 cycle per value that's pushed
    }

    template <int page, uint8_t opCode>
    void OpPUL(uint16_t& stackReg) {
        assert(&stackReg == &S || &stackReg == &U);
        const uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        if (value & BITS(0))
            CC.Value = Pop8(stackReg);
        if (value & BITS(1))
            A = Pop8(stackReg);
        if (value & BITS(2))
            B = Pop8(stackReg);
        if (value & BITS(3))
            DP = Pop8(stackReg);
        if (value & BITS(4))
            X = Pop16(stackReg);
        if (value & BITS(5))
            Y = Pop16(stackReg);
        if (value & BITS(6)) {
            auto& otherStackReg = &stackReg == &S ? U : S;
            otherStackReg = Pop16(stackReg);
        }
        if (value & BITS(7))
            PC = Pop16(stackReg);

        m_cycles += NumBitsSet(value); // 1 cycle per value that's pulled
    }

    template <int page, uint8_t opCode>
    void OpTST(const uint8_t& value) {
        CC.Negative = (value & BITS(7)) != 0;
        CC.Zero = value == 0;
        CC.Overflow = 0;
    }

    template <int page, uint8_t opCode>
    void OpTST() {
        OpTST<page, opCode>(ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>());
    }

    template <int page, uint8_t opCode>
    void OpOR(uint8_t& reg) {
        uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        reg = reg | value;
        // For ORCC, we don't update CC. @TODO: separate function?
        if (&reg != &CC.Value) {
            CC.Negative = (value & BITS(7)) != 0;
            CC.Zero = value == 0;
            CC.Overflow = 0;
        }
    }

    template <int page, uint8_t opCode>
    void OpAND(uint8_t& reg) {
        uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        reg = reg & value;
        // For ANDCC, we don't update CC. @TODO: separate function?
        if (&reg != &CC.Value) {
            CC.Negative = (value & BITS(7)) != 0;
            CC.Zero = value == 0;
            CC.Overflow = 0;
        }
    }

    template <int page, uint8_t opCode>
    void OpCMP(const uint8_t& reg) {
        // Subtract to update CC, but discard result
        uint8_t discard =
            SubtractImpl(reg, ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>(), CC);
        (void)discard;
    }

    template <int page, uint8_t opCode>
    void OpCMP(const uint16_t& reg) {
        uint16_t discard =
            SubtractImpl(reg, ReadOperandValue16<LookupCpuOp(page, opCode).addrMode>(), CC);
        (void)discard;
    }

    template <int page, uint8_t opCode>
    void OpBIT(const uint8_t& reg) {
        uint8_t value = ReadOperandValue8<LookupCpuOp(page, opCode).addrMode>();
        uint8_t result = reg & value;
        CC.Negative = (result & BITS(7)) != 0;
        CC.Zero = result == 0;
        CC.Overflow = 0;
    }

    template <typename CondFunc>
    void OpBranch(CondFunc condFunc) {
        int8_t offset = ReadRelativeOffset8();
        if (condFunc()) {
            PC += offset;
            m_cycles += 1; // Extra cycle if branch is taken
        }
    }

    void OpBSR() {
        int8_t offset = ReadRelativeOffset8();
        Push16(S, PC);
        PC += offset;
    }

    void OpLBSR() {
        int16_t offset = ReadRelativeOffset16();
        Push16(S, PC);
        PC += offset;
    }

    void OpRTS() { PC = Pop16(S); }

    void ExchangeOrTransfer(bool exchange) {
        uint8_t postbyte = ReadPC8();
        assert(!!(postbyte & BITS(3)) ==
               !!(postbyte & BITS(7))); // 8-bit to 8-bit or 16-bit to 16-bit only

        uint8_t src = (postbyte >> 4) & 0b111;
        uint8_t dst = postbyte & 0b111;

        if (postbyte & BITS(3)) {
            assert(src < 4 && dst < 4); // Only first 4 are valid 8-bit register indices
            uint8_t* const reg[]{&A, &B, &CC.Value, &DP};
            if (exchange)
                std::swap(*reg[dst], *reg[src]);
            else
                *reg[dst] = *reg[src];
        } else {
            assert(src < 6 && dst < 6); // Only first 6 are valid 16-bit register indices
            uint16_t* const reg[]{&D, &X, &Y, &U, &S, &PC};
            if (exchange)
                std::swap(*reg[dst], *reg[src]);
            else
                *reg[dst] = *reg[src];
        }
    }

    void OpEXG() { ExchangeOrTransfer(true); }

    void OpTFR() { ExchangeOrTransfer(false); }

    cycles_t ExecuteInstruction() {
        m_cycles = 0;

        auto UnhandledOp = [this](const CpuOp& cpuOp) {
            (void)cpuOp;
            FAIL("Unhandled Op: %s", cpuOp.name);
        };

        int cpuOpPage = 0;
        uint8_t opCodeByte = ReadPC8();
        if (IsOpCodePage1(opCodeByte)) {
            cpuOpPage = 1; //@TODO: 1 cycle (see CpuOpsPage0)
            opCodeByte = ReadPC8();
        } else if (IsOpCodePage2(opCodeByte)) {
            cpuOpPage = 2; //@TODO: 1 cycle (see CpuOpsPage0)
            opCodeByte = ReadPC8();
        }

        const CpuOp& cpuOp = LookupCpuOpRuntime(cpuOpPage, opCodeByte);

        assert(cpuOp.cycles > 0 && "TODO: look at how to handle cycles for this instruction");
        //@TODO: Handle cycle counting for interrupts (SWI[2/3], [F]IRQ, NMI) and RTI
        m_cycles += cpuOp.cycles; // Base cycles for this instruction

        assert(cpuOp.addrMode != AddressingMode::Illegal && "Illegal instruction!");
        assert(cpuOp.addrMode != AddressingMode::Variant &&
               "Page 1/2 instruction, should have read next byte by now");

        switch (cpuOpPage) {
        case 0:
            switch (cpuOp.opCode) {
            case 0x12:
                // NOP
                break;

            case 0x9D:
                OpJSR<0, 0x9D>();
                break;
            case 0xAD:
                OpJSR<0, 0xAD>();
                break;
            case 0xBD:
                OpJSR<0, 0xBD>();
                break;

            // 8-bit LD
            case 0x86:
                OpLD<0, 0x86>(A);
                break;
            case 0x96:
                OpLD<0, 0x96>(A);
                break;
            case 0xA6:
                OpLD<0, 0xA6>(A);
                break;
            case 0xB6:
                OpLD<0, 0xB6>(A);
                break;
            case 0xC6:
                OpLD<0, 0xC6>(B);
                break;
            case 0xD6:
                OpLD<0, 0xD6>(B);
                break;
            case 0xE6:
                OpLD<0, 0xE6>(B);
                break;
            case 0xF6:
                OpLD<0, 0xF6>(B);
                break;
            // 16-bit LD
            case 0x8E:
                OpLD<0, 0x8E>(X);
                break;
            case 0x9E:
                OpLD<0, 0x9E>(X);
                break;
            case 0xAE:
                OpLD<0, 0xAE>(X);
                break;
            case 0xBE:
                OpLD<0, 0xBE>(X);
                break;
            case 0xCC:
                OpLD<0, 0xCC>(D);
                break;
            case 0xDC:
                OpLD<0, 0xDC>(D);
                break;
            case 0xEC:
                OpLD<0, 0xEC>(D);
                break;
            case 0xFC:
                OpLD<0, 0xFC>(D);
                break;
            case 0xCE:
                OpLD<0, 0xCE>(U);
                break;
            case 0xDE:
                OpLD<0, 0xDE>(U);
                break;
            case 0xEE:
                OpLD<0, 0xEE>(U);
                break;
            case 0xFE:
                OpLD<0, 0xFE>(U);
                break;

            // 8-bit ST
            case 0x97:
                OpST<0, 0x97>(A);
                break;
            case 0xA7:
                OpST<0, 0xA7>(A);
                break;
            case 0xB7:
                OpST<0, 0xB7>(A);
                break;
            case 0xD7:
                OpST<0, 0xD7>(B);
                break;
            case 0xE7:
                OpST<0, 0xE7>(B);
                break;
            case 0xF7:
                OpST<0, 0xF7>(B);
                break;
            // 16-bit ST
            case 0x9F:
                OpST<0, 0x9F>(X);
                break;
            case 0xAF:
                OpST<0, 0xAF>(X);
                break;
            case 0xBF:
                OpST<0, 0xBF>(X);
                break;
            case 0xDD:
                OpST<0, 0xDD>(D);
                break;
            case 0xDF:
                OpST<0, 0xDF>(U);
                break;
            case 0xED:
                OpST<0, 0xED>(D);
                break;
            case 0xEF:
                OpST<0, 0xEF>(U);
                break;
            case 0xFD:
                OpST<0, 0xFD>(D);
                break;
            case 0xFF:
                OpST<0, 0xFF>(U);
                break;

            case 0x30:
                OpLEA<0, 0x30>(X);
                break;
            case 0x31:
                OpLEA<0, 0x31>(Y);
                break;
            case 0x32:
                OpLEA<0, 0x32>(S);
                break;
            case 0x33:
                OpLEA<0, 0x33>(U);
                break;

            case 0x8D:
                OpBSR();
                break;
            case 0x17:
                OpLBSR();
                break;

            case 0x24: // BCC (branch if carry clear) or BHS (branch if higher or same)
                OpBranch([this] { return CC.Carry == 0; });
                break;
            case 0x25: // BCS (branch if carry set) or BLO (branch if lower)
                OpBranch([this] { return CC.Carry != 0; });
                break;
            case 0x27: // BEQ (branch if equal)
                OpBranch([this] { return CC.Zero != 0; });
                break;
            case 0x2C: // BGE (branch if greater or equal)
                OpBranch([this] { return (CC.Negative ^ CC.Overflow) == 0; });
                break;
            case 0x2E: // BGT (branch if greater)
                OpBranch([this] { return (CC.Zero | (CC.Negative ^ CC.Overflow)) == 0; });
                break;
            case 0x22: // BHI (branch if higher)
                OpBranch([this] { return (CC.Carry | CC.Zero) == 0; });
                break;
            case 0x2F: // BLE (branch if less or equal)
                OpBranch([this] { return (CC.Zero | (CC.Negative ^ CC.Overflow)) != 0; });
                break;
            case 0x23: // BLS (banch if lower or same)
                OpBranch([this] { return (CC.Carry | CC.Zero) != 0; });
                break;
            case 0x2D: // BLT (branch if less than)
                OpBranch([this] { return (CC.Negative ^ CC.Overflow) != 0; });
                break;
            case 0x2B: // BMI (brach if minus)
                OpBranch([this] { return CC.Negative != 0; });
                break;
            case 0x26: // BNE (branch if not equal)
                OpBranch([this] { return CC.Zero == 0; });
                break;
            case 0x2A: // BPL (branch if plus)
                OpBranch([this] { return CC.Negative == 0; });
                break;
            case 0x20: // BRA (branch always)
                OpBranch([this] { return true; });
                break;
            case 0x21: // BRN (branch never)
                OpBranch([this] { return false; });
                break;
            case 0x28: // BVC (branch if overflow clear)
                OpBranch([this] { return CC.Overflow == 0; });
                break;
            case 0x29: // BVS (branch if overflow set)
                OpBranch([this] { return CC.Overflow != 0; });
                break;

            case 0x1E: // EXG (exchange/swap register values)
                OpEXG();
                break;

            case 0x1F:
                OpTFR();
                break;

            case 0x39:
                OpRTS();
                break;

            case 0x4F:
                OpCLR(A);
                break;
            case 0x5F:
                OpCLR(B);
                break;
            case 0x0F:
                OpCLR<0, 0x0F>();
                break;
            case 0x6F:
                OpCLR<0, 0x6F>();
                break;
            case 0x7F:
                OpCLR<0, 0x7F>();
                break;

            case 0x8B:
                OpADD<0, 0x8B>(A);
                break;
            case 0x9B:
                OpADD<0, 0x9B>(A);
                break;
            case 0xAB:
                OpADD<0, 0xAB>(A);
                break;
            case 0xBB:
                OpADD<0, 0xBB>(A);
                break;
            case 0xC3:
                OpADD<0, 0xC3>(D);
                break;
            case 0xCB:
                OpADD<0, 0xCB>(B);
                break;
            case 0xD3:
                OpADD<0, 0xD3>(D);
                break;
            case 0xDB:
                OpADD<0, 0xDB>(B);
                break;
            case 0xE3:
                OpADD<0, 0xE3>(D);
                break;
            case 0xEB:
                OpADD<0, 0xEB>(B);
                break;
            case 0xF3:
                OpADD<0, 0xF3>(D);
                break;
            case 0xFB:
                OpADD<0, 0xFB>(B);
                break;

            case 0x80:
                OpSUB<0, 0x80>(A);
                break;
            case 0x83:
                OpSUB<0, 0x83>(D);
                break;
            case 0x90:
                OpSUB<0, 0x90>(A);
                break;
            case 0x93:
                OpSUB<0, 0x93>(D);
                break;
            case 0xA0:
                OpSUB<0, 0xA0>(A);
                break;
            case 0xA3:
                OpSUB<0, 0xA3>(D);
                break;
            case 0xB0:
                OpSUB<0, 0xB0>(A);
                break;
            case 0xB3:
                OpSUB<0, 0xB3>(D);
                break;
            case 0xC0:
                OpSUB<0, 0xC0>(B);
                break;
            case 0xD0:
                OpSUB<0, 0xD0>(B);
                break;
            case 0xE0:
                OpSUB<0, 0xE0>(B);
                break;
            case 0xF0:
                OpSUB<0, 0xF0>(B);
                break;

            case 0x89:
                OpADC<0, 0x89>(A);
                break;
            case 0x99:
                OpADC<0, 0x99>(A);
                break;
            case 0xA9:
                OpADC<0, 0xA9>(A);
                break;
            case 0xB9:
                OpADC<0, 0xB9>(A);
                break;
            case 0xC9:
                OpADC<0, 0xC9>(B);
                break;
            case 0xD9:
                OpADC<0, 0xD9>(B);
                break;
            case 0xE9:
                OpADC<0, 0xE9>(B);
                break;
            case 0xF9:
                OpADC<0, 0xF9>(B);
                break;

            // NEG
            case 0x00:
                OpNEG<0, 0x00>();
                break;
            case 0x40:
                OpNEG<0, 0x40>(A);
                break;
            case 0x50:
                OpNEG<0, 0x50>(B);
                break;
            case 0x60:
                OpNEG<0, 0x60>();
                break;
            case 0x70:
                OpNEG<0, 0x70>();
                break;

            // INC
            case 0x0C:
                OpINC<0, 0x0C>();
                break;
            case 0x4C:
                OpINC<0, 0x4C>(A);
                break;
            case 0x5C:
                OpINC<0, 0x5C>(B);
                break;
            case 0x6C:
                OpINC<0, 0x6C>();
                break;
            case 0x7C:
                OpINC<0, 0x7C>();
                break;

            // DEC
            case 0x0A:
                OpDEC<0, 0x0A>();
                break;
            case 0x4A:
                OpDEC<0, 0x4A>(A);
                break;
            case 0x5A:
                OpDEC<0, 0x5A>(B);
                break;
            case 0x6A:
                OpDEC<0, 0x6A>();
                break;
            case 0x7A:
                OpDEC<0, 0x7A>();
                break;

            // ASR
            case 0x07:
                OpASR<0, 0x07>();
                break;
            case 0x47:
                OpASR<0, 0x47>(A);
                break;
            case 0x57:
                OpASR<0, 0x57>(B);
                break;
            case 0x67:
                OpASR<0, 0x67>();
                break;
            case 0x77:
                OpASR<0, 0x77>();
                break;

            // LSL/ASL
            case 0x08:
                OpASL<0, 0x08>();
                break;
            case 0x48:
                OpASL<0, 0x48>(A);
                break;
            case 0x58:
                OpASL<0, 0x58>(B);
                break;
            case 0x68:
                OpASL<0, 0x68>();
                break;
            case 0x78:
                OpASL<0, 0x78>();
                break;

            // LSR
            case 0x04:
                OpLSR<0, 0x04>();
                break;
            case 0x44:
                OpLSR<0, 0x44>(A);
                break;
            case 0x54:
                OpLSR<0, 0x54>(B);
                break;
            case 0x64:
                OpLSR<0, 0x64>();
                break;
            case 0x74:
                OpLSR<0, 0x74>();
                break;

            // COM
            case 0x03:
                OpCOM<0, 0x03>();
                break;
            case 0x43:
                OpCOM<0, 0x43>(A);
                break;
            case 0x53:
                OpCOM<0, 0x53>(B);
                break;
            case 0x63:
                OpCOM<0, 0x63>();
                break;
            case 0x73:
                OpCOM<0, 0x73>();
                break;

            // JMP
            case 0x0E:
                OpJMP<0, 0x0E>();
                break;
            case 0x6E:
                OpJMP<0, 0x6E>();
                break;
            case 0x7E:
                OpJMP<0, 0x7E>();
                break;

            // PSH/PUL
            case 0x34: // PSHS
                OpPSH<0, 0x34>(S);
                break;
            case 0x35: // PULS
                OpPUL<0, 0x35>(S);
                break;
            case 0x36: // PSHU
                OpPSH<0, 0x36>(U);
                break;
            case 0x37: // PULU
                OpPUL<0, 0x37>(U);
                break;

            // TST
            case 0x0D:
                OpTST<0, 0x0D>();
                break;
            case 0x4D:
                OpTST<0, 0x4D>(A);
                break;
            case 0x5D:
                OpTST<0, 0x5D>(B);
                break;
            case 0x6D:
                OpTST<0, 0x6D>();
                break;
            case 0x7D:
                OpTST<0, 0x7D>();
                break;

            // ORA/ORB
            case 0x8A:
                OpOR<0, 0x8A>(A);
                break;
            case 0x9A:
                OpOR<0, 0x9A>(A);
                break;
            case 0xAA:
                OpOR<0, 0xAA>(A);
                break;
            case 0xBA:
                OpOR<0, 0xBA>(A);
                break;
            case 0xCA:
                OpOR<0, 0xCA>(B);
                break;
            case 0xDA:
                OpOR<0, 0xDA>(B);
                break;
            case 0xEA:
                OpOR<0, 0xEA>(B);
                break;
            case 0xFA:
                OpOR<0, 0xFA>(B);
                break;
            case 0x1A:
                OpOR<0, 0x1A>(CC.Value);
                break;

            // AND
            case 0x1C:
                OpAND<0, 0x1C>(CC.Value);
                break;
            case 0x84:
                OpAND<0, 0x84>(A);
                break;
            case 0x94:
                OpAND<0, 0x94>(A);
                break;
            case 0xA4:
                OpAND<0, 0xA4>(A);
                break;
            case 0xB4:
                OpAND<0, 0xB4>(A);
                break;
            case 0xC4:
                OpAND<0, 0xC4>(B);
                break;
            case 0xD4:
                OpAND<0, 0xD4>(B);
                break;
            case 0xE4:
                OpAND<0, 0xE4>(B);
                break;
            case 0xF4:
                OpAND<0, 0xF4>(B);
                break;

            // CMP
            case 0x81:
                OpCMP<0, 0x81>(A);
                break;
            case 0x8C:
                OpCMP<0, 0x8C>(X);
                break;
            case 0x91:
                OpCMP<0, 0x91>(A);
                break;
            case 0x9C:
                OpCMP<0, 0x9C>(X);
                break;
            case 0xA1:
                OpCMP<0, 0xA1>(A);
                break;
            case 0xAC:
                OpCMP<0, 0xAC>(X);
                break;
            case 0xB1:
                OpCMP<0, 0xB1>(A);
                break;
            case 0xBC:
                OpCMP<0, 0xBC>(X);
                break;
            case 0xC1:
                OpCMP<0, 0xC1>(B);
                break;
            case 0xD1:
                OpCMP<0, 0xD1>(B);
                break;
            case 0xE1:
                OpCMP<0, 0xE1>(B);
                break;
            case 0xF1:
                OpCMP<0, 0xF1>(B);
                break;

            // BIT
            case 0x85:
                OpBIT<0, 0x85>(A);
                break;
            case 0x95:
                OpBIT<0, 0x95>(A);
                break;
            case 0xA5:
                OpBIT<0, 0xA5>(A);
                break;
            case 0xB5:
                OpBIT<0, 0xB5>(A);
                break;
            case 0xC5:
                OpBIT<0, 0xC5>(B);
                break;
            case 0xD5:
                OpBIT<0, 0xD5>(B);
                break;
            case 0xE5:
                OpBIT<0, 0xE5>(B);
                break;
            case 0xF5:
                OpBIT<0, 0xF5>(B);
                break;

            default:
                UnhandledOp(cpuOp);
            }
            break;

        case 1:
            switch (cpuOp.opCode) {
            // 16-bit LD
            case 0x8E:
                OpLD<1, 0x8E>(Y);
                break;
            case 0x9E:
                OpLD<1, 0x9E>(Y);
                break;
            case 0xAE:
                OpLD<1, 0xAE>(Y);
                break;
            case 0xBE:
                OpLD<1, 0xBE>(Y);
                break;
            case 0xCE:
                OpLD<1, 0xCE>(S);
                break;
            case 0xDE:
                OpLD<1, 0xDE>(S);
                break;
            case 0xEE:
                OpLD<1, 0xEE>(S);
                break;
            case 0xFE:
                OpLD<1, 0xFE>(S);
                break;

            // 16-bit ST
            case 0x9F:
                OpST<1, 0x9F>(Y);
                break;
            case 0xAF:
                OpST<1, 0xAF>(Y);
                break;
            case 0xBF:
                OpST<1, 0xBF>(Y);
                break;
            case 0xDF:
                OpST<1, 0xDF>(S);
                break;
            case 0xEF:
                OpST<1, 0xEF>(S);
                break;
            case 0xFF:
                OpST<1, 0xFF>(S);
                break;

            // CMP
            case 0x83:
                OpCMP<1, 0x83>(D);
                break;
            case 0x8C:
                OpCMP<1, 0x8C>(Y);
                break;
            case 0x93:
                OpCMP<1, 0x93>(D);
                break;
            case 0x9C:
                OpCMP<1, 0x9C>(Y);
                break;
            case 0xA3:
                OpCMP<1, 0xA3>(D);
                break;
            case 0xAC:
                OpCMP<1, 0xAC>(Y);
                break;
            case 0xB3:
                OpCMP<1, 0xB3>(D);
                break;
            case 0xBC:
                OpCMP<1, 0xBC>(Y);
                break;

            default:
                UnhandledOp(cpuOp);
            }
            break;

        case 2:
            switch (cpuOp.opCode) {
            case 0x00:
                UnhandledOp(cpuOp);
                break;

            // CMP
            case 0x83:
                OpCMP<2, 0x83>(U);
                break;
            case 0x8C:
                OpCMP<2, 0x8C>(S);
                break;
            case 0x93:
                OpCMP<2, 0x93>(U);
                break;
            case 0x9C:
                OpCMP<2, 0x9C>(S);
                break;
            case 0xA3:
                OpCMP<2, 0xA3>(U);
                break;
            case 0xAC:
                OpCMP<2, 0xAC>(S);
                break;
            case 0xB3:
                OpCMP<2, 0xB3>(U);
                break;
            case 0xBC:
                OpCMP<2, 0xBC>(S);
                break;

            default:
                UnhandledOp(cpuOp);
            }
            break;
        }

        return m_cycles;
    }
};

Cpu::Cpu()
    : m_impl(std::make_unique<CpuImpl>()) {}

Cpu::~Cpu() {}

void Cpu::Init(MemoryBus& memoryBus) {
    m_impl->Init(memoryBus);
}

void Cpu::Reset(uint16_t initialPC) {
    m_impl->Reset(initialPC);
}

cycles_t Cpu::ExecuteInstruction() {
    return m_impl->ExecuteInstruction();
}

const CpuRegisters& Cpu::Registers() {
    return *m_impl;
}
