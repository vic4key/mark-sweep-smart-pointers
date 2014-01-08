#ifndef GCPTR_H
#define GCPTR_H

#include <utility>
#include <type_traits>

// Platform definitions (for GCC 4.6)
template <typename T>
constexpr bool use_destructor() { return !std::has_trivial_destructor<T>::value; }
template <typename T>
constexpr bool use_default_constructor() { return !std::has_trivial_default_constructor<T>::value; }

namespace gcptr
{
	// Array destructors
	typedef void (*destructor)(void *obj, unsigned nelems);

	// Forward declarations
	struct mblock;
	class basic_ptr;
	template <typename T> class ptr;

	// Garbage collection. Returns amount of freed memory.
	unsigned collect();

	// Get/set the threshold of memory allocated since last collection necessary to force a new one.
	unsigned collect_threshold(unsigned newthr = 0);

	// Untyped basic smart pointer
	class basic_ptr
	{
		private:

			// List handling.
			basic_ptr *next;
			basic_ptr *prev;
			void link();
			void unlink();

			// Used by the garbage collector
			static void mark(basic_ptr *list);

		public:

			// Attach this to the same object array as another smart pointer. Returns true if attached.
			bool attach(const basic_ptr &p);

			// Attach this to the most nested object array in construction, if any. Returns true if attached.
			bool attach();

			// Tells whether this is attached.
			bool is_attached() const;

			// Detach.
			void detach();

			// Collect garbage if necessary, or unconditionally. Returns amount of freed memory.
			static unsigned gc(bool unconditional);

		protected:

			// Constructors, assignment operators and destructor.
			basic_ptr();
			basic_ptr(const basic_ptr &src);
			basic_ptr &operator =(const basic_ptr &src);
			basic_ptr(void *src);
			basic_ptr &operator =(void *src);
			basic_ptr(const basic_ptr &src, void *p);
			~basic_ptr();

			// Check that this can be dereferenced:
			// (1) Pointer value is not null.
			// (2) If attached, it points into the attached object array.
			void check() const;

			// Allocation of garbage-collected object arrays.
			void *alloc_begin(unsigned nelems, unsigned elem_size, destructor destr, bool zero);
			void alloc_end(unsigned nconstructed);

			// Pointer to memory block, null if not attached.
			mblock *mem;

			// Pointer value
			void *pval;
	};

	// Initialization policy constants
	struct initspec_t { bool zero; };
	const initspec_t init_undef	{ false };
	const initspec_t init_zero { true };

	// Smart pointer exceptions
	class ptr_exception
	{
		public:

			ptr_exception(const char *s = "ptr exception");
			const char *what();
		
		private:

			const char *msg;
	};
	
	// Smart pointer
	template <typename T> class ptr : public basic_ptr
	{
		public:

			// Default constructor
			ptr() = default;

			// Construct from a real pointer
			ptr(T *p) : basic_ptr(p) { }

			// Assign a real pointer
			ptr &operator =(T *p)
			{ 
				basic_ptr::operator =(p); 
				return *this; 
			}

			// Construct from a smart pointer of a different type (casting)
			template <typename U> explicit ptr(const ptr<U> &src) : basic_ptr(src) { }

			// Construct from a smart pointer to an object and a pointer to class member.
			// Point to the member of the object and get the same attachment as the source smart pointer.
			template <typename U> ptr(const ptr<U> &src, T U::*pm) : basic_ptr(src, &(src->*pm)) { }

			// Construct from a smart pointer to an object or array and a real pointer to a
			// member or array element. Point to the member or element and get the same attachment
			// as the source smart pointer.
			template <typename U> ptr(const ptr<U> &src, T *p) : basic_ptr(src, p) { }

			// Pointer operations
			operator T *() const { return cref(); }
			T *operator ->() const { check(); return cref(); }
			T &operator *() const { check(); return *cref(); }
			T &operator [](int n) const { check(); return cref()[n]; }
			ptr &operator ++() { ++ref(); return *this; }
			const ptr operator ++(int) { ptr p = *this; ++ref(); return p; }
			ptr &operator --() { --ref(); return *this; }
			const ptr operator --(int) { ptr p = *this; --ref(); return p; }
			ptr &operator +=(int n) { ref() += n; return *this; }
			ptr &operator -=(int n) { ref() -= n; return *this; }
			const ptr operator +(int n) const { return ptr(*this, cref() + n); }
			const ptr operator -(int n) const { return ptr(*this, cref() - n); }
			const int operator -(const ptr &p) const { return cref() - p.cref(); }

			// Allocate an array with one or more arguments constructor arguments.
			template <typename U, typename... V>
			void alloc_array(unsigned nelems, U&& first, V&&... rest)
			{ 
				unsigned n = 0;
				try
				{ 
					T *t = static_cast<T *>(alloc_begin(nelems, sizeof(T), destr, false));
					for ( ; n < nelems ; n++ )
						new(t++) T(std::forward<U>(first), std::forward<V>(rest)...);
					alloc_end(n);
				}
				catch (...)
				{ 
					alloc_end(n);
					throw; 
				}
			}

			// Allocate an array without arguments. If the constant init_zero is passed as
			// second argument, object memory is initialized to zero.
			void alloc_array(unsigned nelems, initspec_t init = init_undef)
			{
				unsigned n = 0;
				try
				{ 
					T *t = static_cast<T *>(alloc_begin(nelems, sizeof(T), destr, init.zero));
					if ( use_default_constructor<T>() )						
						for ( ; n < nelems ; n++ )
							new(t++) T();
					else
						n = nelems;
					alloc_end(n);
				}
				catch (...)
				{ 
					alloc_end(n);
					throw; 
				}
			}

			// Allocate a single object with one or more constructor arguments.
			template <typename U, typename... V>
			void alloc(U&& first, V&&... rest)
			{ 
				try
				{ 
					T *t = static_cast<T *>(alloc_begin(1, sizeof(T), destr, false));
					new(t) T(std::forward<U>(first), std::forward<V>(rest)...);
					alloc_end(1);
				}
				catch (...)
				{ 
					alloc_end(0);
					throw; 
				}
			}

			// Allocate a single object without arguments. If the constant init_zero is passed as
			// second argument, object memory is initialized to zero.
			void alloc(initspec_t init = init_undef)
			{
				try
				{ 
					T *t = static_cast<T *>(alloc_begin(1, sizeof(T), destr, init.zero));
					if ( use_default_constructor<T>() )						
						new(t) T();
					alloc_end(1);
				}
				catch (...)
				{ 
					alloc_end(0);
					throw; 
				}
			}

		private:

			// Pointer value as T *.
			T * &ref() { return reinterpret_cast<T * &>(pval); }
			T * const &cref() const { return reinterpret_cast<T * const &>(pval); }

			// Array destructor
			static void destroy(void *p, unsigned nelems)
			{ 
				T *t = static_cast<T *>(p);
				while ( nelems-- )
					try
					{
						(t++)->~T();
					}
					catch (...)
					{
						// Ignore exceptions
					}
			}

			// Use array destructor only for types with non-trivial destructors
			constexpr static destructor destr = use_destructor<T>() ? destroy : nullptr;
	};
	
}

#endif

