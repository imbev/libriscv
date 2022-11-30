#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "rvc.hpp"

namespace riscv
{
	struct UnalignedLoad32 {
		uint16_t data[2];
		operator uint32_t() {
			return data[0] | uint32_t(data[1]) << 16;
		}
	};
	struct AlignedLoad16 {
		uint16_t data;
		operator uint32_t() { return data; }
	};
	static inline rv32i_instruction read_instruction(
		const uint8_t* exec_segment, uint64_t pc, uint64_t end_pc)
	{
		if (pc + 4 <= end_pc)
			return {*(UnalignedLoad32 *)&exec_segment[pc]};
		else
			return {*(AlignedLoad16 *)&exec_segment[pc]};
	}

	static constexpr uint32_t FASTSIM_BLOCK_END = 0xFFFF;

	template <int W>
	static bool is_regular_compressed(uint16_t instr) {
		const rv32c_instruction ci { instr };
		#define CI_CODE(x, y) ((x << 13) | (y))
		switch (ci.opcode()) {
		case CI_CODE(0b001, 0b01):
			if constexpr (W >= 8) return true; // C.ADDIW
			return false; // C.JAL 32-bit
		case CI_CODE(0b101, 0b01): // C.JMP
		case CI_CODE(0b110, 0b01): // C.BEQZ
		case CI_CODE(0b111, 0b01): // C.BNEZ
			return false;
		case CI_CODE(0b100, 0b10): { // VARIOUS
				const bool topbit = ci.whole & (1 << 12);
				if (!topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JR rd
				} else if (topbit && ci.CR.rd != 0 && ci.CR.rs2 == 0) {
					return false; // C.JALR ra, rd+0
				} // TODO: Handle C.EBREAK
				return true;
			}
		default:
			return true;
		}
	}

	// While we do somewhat care about the precise amount of instructions per block,
	// there is never really going to be any one block with more than 255 raw instructions.
	// Still, we do care about making progress towards the instruction limits.
	inline uint8_t overflow_checked_instr_count(size_t count) {
		return (count > 255) ? 255 : count;
	}

	template <int W>
	static void realize_fastsim(
		address_type<W> base_pc, address_type<W> last_pc,
		const uint8_t* exec_segment, DecoderData<W>* exec_decoder)
	{
		if constexpr (compressed_enabled)
		{
			// Go through entire executable segment and measure lengths
			// Record entries while looking for jumping instruction, then
			// fill out data and opcode lengths previous instructions.
			std::vector<DecoderData<W>*> data;
			address_type<W> pc = base_pc;
			while (pc < last_pc) {
				size_t datalength = 0;
				address_type<W> block_pc = pc;
				while (pc < last_pc) {
					auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
					data.push_back(&entry);

					const auto instruction = read_instruction(
						exec_segment, pc, last_pc);
					const auto opcode = instruction.opcode();
					const auto length = instruction.length();
					pc += length;
					datalength += length / 2;

					// All opcodes that can modify PC
					if (length == 2)
					{
						if (!is_regular_compressed<W>(instruction.half[0]))
							break;
					} else {
						if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
							|| opcode == RV32I_JAL || opcode == RV32I_JALR
							|| opcode == RV32I_AUIPC || entry.instr == FASTSIM_BLOCK_END)
							break;
					}
				}
				for (size_t i = 0; i < data.size(); i++) {
					const auto instruction = read_instruction(
						exec_segment, block_pc, last_pc);
					const auto length = instruction.length();
					block_pc += length;
					auto* entry = data[i];
					// Ends at *last instruction*
					entry->idxend = datalength;
					entry->opcode_length = length;
					// XXX: We have to pack the instruction count by combining it with the cb length
					// in order to avoid overflows on large code blocks. The code block length
					// has been sufficiently large to avoid overflows in all executables tested.
					entry->instr_count = overflow_checked_instr_count(datalength - (data.size() - i));
					datalength -= length / 2;
				}
				data.clear();
			}
		} else { // !compressed_enabled
			// Count distance to next branching instruction backwards
			// and fill in idxend for all entries along the way.
			unsigned idxend = 0;
			address_type<W> pc = last_pc - 4;
			while (pc >= base_pc)
			{
				const auto instruction = read_instruction(
					exec_segment, pc, last_pc);
				auto& entry = exec_decoder[pc / DecoderCache<W>::DIVISOR];
				const auto opcode = instruction.opcode();

				// All opcodes that can modify PC and stop the machine
				if (opcode == RV32I_BRANCH || opcode == RV32I_SYSTEM
					|| opcode == RV32I_JAL || opcode == RV32I_JALR
					|| opcode == RV32I_AUIPC || entry.instr == FASTSIM_BLOCK_END)
					idxend = 0;
				// Ends at *one instruction before* the block ends
				entry.idxend = idxend;
				// Increment after, idx becomes block count - 1
				idxend ++;

				pc -= 4;
			}
		}
	}

	template <int W> RISCV_INTERNAL
	void Memory<W>::generate_decoder_cache(
		[[maybe_unused]] const MachineOptions<W>& options,
		DecodedExecuteSegment<W>& exec)
		//address_t pbase, address_t addr, size_t len)
	{
		const auto pbase = exec.pagedata_base();
		const auto addr  = exec.exec_begin();
		const auto len   = exec.exec_end() - exec.exec_begin();

		constexpr size_t PMASK = Page::size()-1;
		const size_t prelen  = addr - pbase;
		const size_t midlen  = len + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;

		const size_t n_pages = plen / Page::size();
		if (n_pages == 0) {
			throw MachineException(INVALID_PROGRAM,
				"Program produced empty decoder cache");
		}
		// there could be an old cache from a machine reset
		auto* decoder_cache = exec.create_decoder_cache(
			new DecoderCache<W> [n_pages],
			n_pages * sizeof(DecoderCache<W>));
		auto* exec_decoder = 
			decoder_cache[0].get_base() - pbase / DecoderCache<W>::DIVISOR;
		exec.set_decoder(exec_decoder);

		// PC-relative pointer to instruction bits
		auto* exec_segment = exec.exec_data();

#ifdef RISCV_BINARY_TRANSLATION
		// We do not support binary translation for RV128I
		// Also, don't run the translator again (for now)
		if (W != 16 && !is_binary_translated()) {
			// Attempt to load binary translation
			// Also, fill out the binary translation SO filename for later
			std::string bintr_filename;
			machine().cpu.load_translation(options, &bintr_filename);

			if (!machine().is_binary_translated())
			{
				// This can be improved somewhat, by fetching them on demand
				// instead of building a vector of the whole execute segment.
				std::vector<TransInstr<W>> ipairs;
				ipairs.reserve(len / 4);

				for (address_t dst = addr; dst < addr + len; dst += 4)
				{
					// Load unaligned instruction from execute segment
					const rv32i_instruction instruction { *(UnalignedLoad32*) &exec_segment[dst] };
					ipairs.push_back({instruction.whole});
				}
				machine().cpu.try_translate(
					options, bintr_filename, addr, std::move(ipairs));
			}
		} // W != 16
	#endif

		// When compressed instructions are enabled, many decoder
		// entries are illegal because they between instructions.
		bool was_full_instruction = true;

		/* Generate all instruction pointers for executable code.
		   Cannot step outside of this area when pregen is enabled,
		   so it's fine to leave the boundries alone. */
		address_t dst = addr;
		const address_t end_addr = addr + len;
		for (; dst < addr + len;)
		{
			auto& entry = exec_decoder[dst / DecoderCache<W>::DIVISOR];
			entry.instr = 0x0;
			entry.idxend = 0;

			// Load unaligned instruction from execute segment
			const auto instruction = read_instruction(
				exec_segment, dst, end_addr);
			rv32i_instruction rewritten = instruction;

#ifdef RISCV_BINARY_TRANSLATION
			if (machine().is_binary_translated()) {
				if (entry.isset()) {
					// With fastsim we pretend the original opcode is JAL,
					// which breaks the fastsim loop. In all cases, continue.
					entry.instr = FASTSIM_BLOCK_END;
					dst += 4;
					continue;
				}
			}
#endif // RISCV_BINARY_TRANSLATION

			// Insert decoded instruction into decoder cache
			Instruction<W> decoded;
			// The rewriter can rewrite full instructions, so lets only
			// invoke it when we have a decoder cache with full instructions.
			if (!threaded_simulator_enabled && decoder_rewriter_enabled) {
				// Improve many instruction handlers by rewriting instructions
				decoded = CPU<W>::decode_rewrite(dst, rewritten);
			} else {
				decoded = CPU<W>::decode(instruction);
			}
			entry.set_handler(decoded);

			// Cache the (modified) instruction bits
#ifdef RISCV_THREADED
			auto bytecode = CPU<W>::computed_index_for(instruction);
			if constexpr (decoder_rewriter_enabled) {
				bytecode = machine().cpu.threaded_rewrite(bytecode, dst, rewritten);
			}
			entry.set_bytecode(bytecode);
#endif
			entry.instr = rewritten.whole;

			// Increment PC after everything
			if constexpr (compressed_enabled) {
				// With compressed we always step forward 2 bytes at a time
				dst += 2;
				if (was_full_instruction) {
					// For it to be a full instruction again,
					// the length needs to match.
					was_full_instruction = (instruction.length() == 2);
				} else {
					// If it wasn't a full instruction last time, it
					// will for sure be one now.
					was_full_instruction = true;
				}
			} else
				dst += 4;
		}

		realize_fastsim<W>(addr, dst, exec_segment, exec_decoder);
	}

	template <int W> RISCV_INTERNAL
	size_t DecoderData<W>::handler_index_for(Handler new_handler)
	{
		for (size_t i = 1; i < instr_handlers.size(); i++) {
			auto& handler = instr_handlers[i];
			if (handler == new_handler)
				return i;
			else if (handler == nullptr) {
				handler = new_handler;
				return i;
			}
		}
		throw MachineException(MAX_INSTRUCTIONS_REACHED,
			"Not enough instruction handler space", instr_handlers.size());
	}

	// Moved here to work around a GCC bug
	template <int W> RISCV_INTERNAL
	DecodedExecuteSegment<W>& Memory<W>::create_execute_segment(
		const MachineOptions<W>& options, const void *vdata, address_t vaddr, size_t exlen)
	{
		constexpr address_t PMASK = Page::size()-1;
		const address_t pbase = vaddr & ~PMASK;
		const size_t prelen  = vaddr - pbase;
		const size_t midlen  = exlen + prelen;
		const size_t plen = (midlen + PMASK) & ~PMASK;
		const size_t postlen = plen - midlen;
		//printf("Addr 0x%X Len %zx becomes 0x%X->0x%X PRE %zx MIDDLE %zu POST %zu TOTAL %zu\n",
		//	vaddr, exlen, pbase, pbase + plen, prelen, exlen, postlen, plen);
		if (UNLIKELY(prelen > plen || prelen + exlen > plen)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}
		// An additional wrap-around check because we are adding 12 bytes
		// as well as additional padding to len.
		if (UNLIKELY(pbase + plen < pbase)) {
			throw MachineException(INVALID_PROGRAM, "Segment virtual base was bogus");
		}

		// Create the whole executable memory range
		m_exec.emplace_back(pbase, plen, vaddr, exlen);
		auto& current_exec = m_exec.back();

		auto* exec_data = current_exec.exec_data(pbase);
		std::memset(&exec_data[0],      0,     prelen);
		std::memcpy(&exec_data[prelen], vdata, exlen);
		std::memset(&exec_data[prelen + exlen], 0,   postlen);

		this->generate_decoder_cache(options, current_exec);

		return current_exec;
	}

	template <int W>
	DecodedExecuteSegment<W>* Memory<W>::exec_segment_for(address_t vaddr)
	{
		for (auto& segment : m_exec) {
			if (segment.is_within(vaddr)) return &segment;
		}
		return nullptr;
	}

	template <int W>
	const DecodedExecuteSegment<W>* Memory<W>::exec_segment_for(address_t vaddr) const
	{
		return const_cast<Memory<W>*>(this)->exec_segment_for(vaddr);
	}

	template <int W>
	void Memory<W>::evict_execute_segments(size_t remaining_size)
	{
		if (m_exec.size() <= remaining_size)
			return;

		while (m_exec.size() > remaining_size) {
			m_exec.pop_back();
		}
		// XXX: Should probably detect if the current execute
		// segment is already active, but this should also be OK.
		if (!m_exec.empty()) {
			machine().cpu.set_execute_segment(&m_exec[0]);
		}
	}

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
} // riscv
