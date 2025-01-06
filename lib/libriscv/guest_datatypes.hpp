#pragma once
#include "native_heap.hpp" // arena()

namespace riscv {

// View into libstdc++'s std::string
template <int W>
struct GuestStdString {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;
	static constexpr std::size_t SSO = 15;

	gaddr_t ptr;
	gaddr_t size;
	union {
		char data[SSO + 1];
		gaddr_t capacity;
	};

	constexpr GuestStdString() noexcept : ptr(0), size(0), capacity(0) {}
	GuestStdString(machine_t& machine, std::string_view str = "")
		: ptr(0), size(0), capacity(0)
	{
		this->set_string(machine, 0, str);
	}

	bool empty() const noexcept { return size == 0; }

	std::string to_string(machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// Copy the string from guest memory
		const auto view = machine.memory.memview(ptr, size);
		return std::string(view.data(), view.size());
	}

	std::string_view to_view(machine_t& machine, std::size_t max_len = 16UL << 20) const
	{
		if (this->size <= SSO)
			return std::string_view(data, size);
		else if (this->size > max_len)
			throw std::runtime_error("Guest std::string too large (size > 16MB)");
		// View the string from guest memory
		return machine.memory.memview(ptr, size);
	}

	void set_string(machine_t& machine, gaddr_t self, const void* str, std::size_t len)
	{
		this->free(machine);

		if (len <= SSO)
		{
			this->ptr = self + offsetof(GuestStdString, data);
			this->size = len;
			std::memcpy(this->data, str, len);
			this->data[len] = '\0';
		}
		else
		{
			this->ptr = machine.arena().malloc(len);
			this->size = len;
			this->capacity = len;
			machine.copy_to_guest(this->ptr, str, len);
		}
	}
	void set_string(machine_t& machine, gaddr_t self, std::string_view str)
	{
		this->set_string(machine, self, str.data(), str.size());
	}

	void move(gaddr_t self)
	{
		if (size <= SSO) {
			this->ptr = self + offsetof(GuestStdString, data);
		}
	}

	void free(machine_t& machine)
	{
		if (size > SSO) {
			machine.arena().free(ptr);
		}
		this->ptr = 0;
		this->size = 0;
	}
};

template <int W, typename T> struct GuestStdVector;

template <int W, typename T>
struct is_guest_stdvector : std::false_type {};

template <int W, typename T>
struct is_guest_stdvector<W, GuestStdVector<W, T>> : std::true_type {};

// View into libstdc++ and LLVM libc++ std::vector (same layout)
template <int W, typename T>
struct GuestStdVector {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	gaddr_t ptr_begin;
	gaddr_t ptr_end;
	gaddr_t ptr_capacity;

	constexpr GuestStdVector() noexcept : ptr_begin(0), ptr_end(0), ptr_capacity(0) {}

	GuestStdVector(machine_t& machine, std::size_t elements)
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		auto [array, self] = this->alloc(machine, elements);
		(void)self;
		for (std::size_t i = 0; i < elements; i++) {
			new (&array[i]) T();
		}
		// Set new end only after all elements are constructed
		this->ptr_end = this->ptr_begin + elements * sizeof(T);
	}

	GuestStdVector(machine_t& machine, const std::vector<std::string>& vec)
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		static_assert(std::is_same_v<T, GuestStdString<W>>, "GuestStdVector<T> must be a vector of GuestStdString<W>");
		if (vec.empty())
			return;

		// Specialization for std::vector<std::string>
		auto [array, self] = this->alloc(machine, vec.size());
		(void)self;
		for (std::size_t i = 0; i < vec.size(); i++) {
			T* str = new (&array[i]) T(machine, vec[i]);
			str->move(this->ptr_begin + i * sizeof(T));
		}
		// Set new end only after all elements are constructed
		this->ptr_end = this->ptr_begin + vec.size() * sizeof(T);
	}
	GuestStdVector(machine_t& machine, const std::vector<T>& vec = {})
		: ptr_begin(0), ptr_end(0), ptr_capacity(0)
	{
		if (!vec.empty())
			this->assign(machine, vec);
	}
	template <typename... Args>
	GuestStdVector(machine_t& machine, const std::array<T, sizeof...(Args)>& arr)
		: GuestStdVector(machine, std::vector<T> {arr.begin(), arr.end()})
	{
	}

	gaddr_t data() const noexcept { return ptr_begin; }
	std::size_t size_bytes() const noexcept { return ptr_end - ptr_begin; }
	std::size_t capacity() const noexcept { return ptr_capacity - ptr_begin; }

	std::size_t size() const noexcept {
		return size_bytes() / sizeof(T);
	}
	bool empty() const noexcept {
		return size() == 0;
	}

	T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return as_array(machine, max_bytes)[index];
	}
	const T& at(machine_t& machine, std::size_t index, std::size_t max_bytes = 16UL << 20) const {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return as_array(machine, max_bytes)[index];
	}

	void push_back(machine_t& machine, T&& value) {
		if (size_bytes() >= capacity())
			throw std::runtime_error("Guest std::vector has reached capacity");
		T* array = machine.memory.template memarray<T>(this->data(), size() + 1);
		new (&array[size()]) T(std::move(value));
		this->ptr_end += sizeof(T);
	}
	void push_back(machine_t& machine, const T& value) {
		if (size_bytes() >= capacity())
			throw std::runtime_error("Guest std::vector has reached capacity");
		T* array = machine.memory.template memarray<T>(this->data(), size() + 1);
		new (&array[size()]) T(value);
		this->ptr_end += sizeof(T);
	}

	// Specialization for std::string_view
	void push_back(machine_t& machine, std::string_view value) {
		static_assert(std::is_same_v<T, GuestStdString<W>>, "GuestStdVector: T must be a GuestStdString<W>");
		this->push_back(machine, GuestStdString<W>(machine, value));
	}
	// Specialization for std::vector<U>
	template <typename U>
	void push_back(machine_t& machine, const std::vector<U>& value) {
		static_assert(is_guest_stdvector<W, T>::value, "GuestStdVector: T must be a GuestStdVector itself");
		this->push_back(machine, GuestStdVector<W, U>(machine, value));
	}

	void pop_back(machine_t& machine) {
		if (size() == 0)
			throw std::out_of_range("Guest std::vector is empty");
		this->free_element(machine, size() - 1);
		this->ptr_end -= sizeof(T);
	}

	void clear(machine_t& machine) {
		for (std::size_t i = 0; i < size(); i++)
			this->free_element(machine, i);
		this->ptr_end = this->ptr_begin;
	}

	gaddr_t address_at(std::size_t index) const {
		if (index >= size())
			throw std::out_of_range("Guest std::vector index out of range");
		return ptr_begin + index * sizeof(T);
	}

	T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest std::vector has size > max_bytes");
		return machine.memory.template memarray<T>(data(), size());
	}
	const T *as_array(const machine_t& machine, std::size_t max_bytes = 16UL << 20) const {
		if (size_bytes() > max_bytes)
			throw std::runtime_error("Guest std::vector has size > max_bytes");
		return machine.memory.template memarray<T>(data(), size());
	}

	// Iterators
	auto begin(machine_t& machine) { return as_array(machine); }
	auto end(machine_t& machine) { return as_array(machine) + size(); }

	std::vector<T> to_vector(const machine_t& machine) const {
		if (size_bytes() > capacity())
			throw std::runtime_error("Guest std::vector has size > capacity");
		// Copy the vector from guest memory
		const size_t elements = size_bytes() / sizeof(T);
		const T *array = machine.memory.template memarray<T>(data(), elements);
		return std::vector<T>(&array[0], &array[elements]);
	}

	void assign(machine_t& machine, const std::vector<T>& vec)
	{
		auto [array, self] = alloc(machine, vec.size());
		(void)self;
		std::copy(vec.begin(), vec.end(), array);
		this->ptr_end = this->ptr_begin + vec.size() * sizeof(T);
	}

	void free(machine_t& machine) {
		if (this->ptr_begin != 0) {
			for (std::size_t i = 0; i < size(); i++)
				this->free_element(machine, i);
			machine.arena().free(this->data());
			this->ptr_begin = 0;
			this->ptr_end = 0;
			this->ptr_capacity = 0;
		}
	}
private:
	std::tuple<T *, gaddr_t> alloc(machine_t& machine, std::size_t elements) {
		this->free(machine);

		this->ptr_begin = machine.arena().malloc(elements * sizeof(T));
		this->ptr_end = this->ptr_begin;
		this->ptr_capacity = this->ptr_begin + elements * sizeof(T);
		return { machine.memory.template memarray<T>(this->data(), elements), this->data() };
	}

	void free_element(machine_t& machine, std::size_t index) {
		if constexpr (std::is_same_v<T, GuestStdString<W>> || is_guest_stdvector<W, T>::value) {
			this->at(machine, index).free(machine);
		} else {
			this->at(machine, index).~T();
		}
	}
};

template <int W, typename T>
struct ScopedArenaObject {
	using gaddr_t = riscv::address_type<W>;
	using machine_t = riscv::Machine<W>;

	template <typename... Args>
	ScopedArenaObject(machine_t& machine, Args&&... args)
		: m_machine(&machine)
	{
		this->m_addr = m_machine->arena().malloc(sizeof(T));
		if (this->m_addr == 0) {
			throw std::bad_alloc();
		}
		this->m_ptr = m_machine->memory.template memarray<T>(this->m_addr, 1);
		// Adjust the SSO pointer if the object is a std::string
		if constexpr (std::is_same_v<T, GuestStdString<W>>) {
			new (m_ptr) T(machine, std::forward<Args>(args)...);
			this->m_ptr->move(this->m_addr);
		} else if constexpr (is_guest_stdvector<W, T>::value) {
			new (m_ptr) T(machine, std::forward<Args>(args)...);
		} else {
			// Construct the object in place (as if trivially constructible)
			new (m_ptr) T{std::forward<Args>(args)...};
		}
	}

	~ScopedArenaObject() {
		this->free_standard_types();
		m_machine->arena().free(this->m_addr);
	}

	T& operator*() { return *m_ptr; }
	T* operator->() { return m_ptr; }

	gaddr_t address() const { return m_addr; }

	ScopedArenaObject& operator=(const ScopedArenaObject&) = delete;

	ScopedArenaObject& operator=(const T& other) {
		// It's not possible for m_addr to be 0 here, as it would have thrown in the constructor
		this->free_standard_types();
		this->m_ptr = m_machine->memory.template memarray<T>(this->m_addr, 1);
		new (m_ptr) T(other);
		return *this;
	}

	// Special case for std::string
	ScopedArenaObject& operator=(std::string_view other) {
		static_assert(std::is_same_v<T, GuestStdString<W>>, "ScopedArenaObject<T> must be a GuestStdString<W>");
		this->m_ptr->set_string(*m_machine, this->m_addr, other.data(), other.size());
		return *this;
	}

	// Special case for std::vector
	template <typename U>
	ScopedArenaObject& operator=(const std::vector<U>& other) {
		static_assert(std::is_same_v<T, GuestStdVector<W, U>>, "ScopedArenaObject<T> must be a GuestStdVector<W, U>");
		this->m_ptr->assign(*m_machine, other);
		return *this;
	}

	ScopedArenaObject& operator=(ScopedArenaObject&& other) {
		this->free_standard_types();
		this->m_machine = other.m_machine;
		this->m_ptr = other.m_ptr;
		// Swap the addresses (to guarantee that m_addr is always valid)
		std::swap(this->m_addr, other.m_addr);
		other.m_ptr = nullptr;
		return *this;
	}

private:
	void free_standard_types() {
		if constexpr (is_guest_stdvector<W, T>::value || std::is_same_v<T, GuestStdString<W>>) {
			if (this->m_ptr) {
				this->m_ptr->free(*this->m_machine);
			}
		}
	}

	T*      m_ptr  = nullptr;
	gaddr_t m_addr = 0;
	machine_t* m_machine;
};

template <int W, typename T>
struct is_scoped_guest_object : std::false_type {};

template <int W, typename T>
struct is_scoped_guest_object<W, ScopedArenaObject<W, T>> : std::true_type {};

template <int W, typename T>
struct is_scoped_guest_stdvector : std::false_type {};

template <int W, typename T>
struct is_scoped_guest_stdvector<W, ScopedArenaObject<W, GuestStdVector<W, T>>> : std::true_type {};

} // namespace riscv
