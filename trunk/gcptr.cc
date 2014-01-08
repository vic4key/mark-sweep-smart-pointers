#include "gcptr.h"

#include <mutex>
#include <algorithm>

using namespace std;

#define GC_DEBUG	true

// Debugger	
#if GC_DEBUG
	#include <iostream>
	static mutex debug_m;
	#define debug(x) do 										\
	{															\
		debug_m.lock();											\
		cout << __FUNCTION__ << ": " << x << endl;				\
		debug_m.unlock();										\
	}															\
	while (false)
#else
	#define debug(x)
#endif

// Platform definitions (for GCC 4.6)
#define TLS			__thread				// Should be 'thread_local' for C++11.

namespace
{
	// Garbage collection globals
	unsigned threshold = 100 * 1024;		// Allocated memory threshold.
	unsigned allocated;						// Memory allocated since last collection.
	recursive_mutex gc_m;					// Serialize GC
}

namespace gcptr
{
	/////////////////////////
	// Memory block header //
	/////////////////////////

	struct mblock
	{
		destructor destroy;			// Object array destructor
		basic_ptr *members;			// Member smart pointers
		mblock *next;				// Next in list 
		unsigned nelems;			// Number of elements in object array
		unsigned objsize;			// Size of object area
		bool active;				// Block is candidate for GC
		bool marked;				// Block is accessible

		mblock(unsigned nels, unsigned size, destructor destr) : destroy(destr), members(nullptr),
			nelems(nels), objsize(size), active(false), marked(false) { }

		~mblock() { if ( destroy ) destroy(obj(), nelems); }

		// Define the size of this structure so that the object area is maximally aligned.
		constexpr static unsigned size() { return sizeof(aligned_storage<sizeof(mblock)>::type); }

		// Address of first object
		char *obj() { return reinterpret_cast<char *>(this) + size(); }

		// Is an address contained in the object area?
		bool contains(const void *addr) { return addr >= obj() && addr < obj() + objsize; }
	};	
}

using namespace gcptr;

namespace
{
	// Root smart pointers
	mutex roots_m;						// Serialize the roots list
	basic_ptr *roots;					// Roots list
	
	// Memory block globals
	mutex active_m;						// Serialize the active blocks list
	mblock *active_blocks;				// Active blocks
	TLS mblock *constr_stack;			// Thread-local construction stack
	TLS mblock *new_blocks;				// Thread-local new blocks list

	// Push a block at the head of a list
	inline void push(mblock *mb, mblock *&list)
	{
		mb->next = list;
		list = mb;
	}

	// Pop the block at the head of a list
	inline mblock *pop(mblock *&list)
	{
		mblock *mb = list;
		list = list->next;
		return mb;
	}
}

namespace gcptr
{
	/////////////////////
	// Class basic_ptr //
	/////////////////////

	// Attachment 
	bool basic_ptr::attach(const basic_ptr &p) { return (mem = p.mem) != nullptr; }
	bool basic_ptr::attach() { return (mem = constr_stack) != nullptr; }
	bool basic_ptr::is_attached() const { return mem != nullptr; }
	void basic_ptr::detach() { mem = nullptr; }

	// Garbage collector
	unsigned basic_ptr::gc(bool unconditional)
	{
		static bool busy;

		// Exclude other threads
		lock_guard<recursive_mutex> lg(gc_m);

		// Check if we should collect
		if ( busy || (!unconditional && allocated < threshold) )
			return 0;

		busy = true;				// Don't re-enter in same thread
		allocated = 0;

		// Mark accessible blocks.
		active_m.lock();
		roots_m.lock();
		mark(roots);
		roots_m.unlock();

		// Check the active blocks and separate garbage
		mblock *active = nullptr, *garbage = nullptr;
		while ( active_blocks )
		{
			if ( active_blocks->marked )
			{
				active_blocks->marked = false;
				push(pop(active_blocks), active);
			}
			else
				push(pop(active_blocks), garbage);
		}
		active_blocks = active;
		active_m.unlock();

		// Collect garbage
		unsigned freed = 0;
		while ( garbage )
		{
			mblock *mb = pop(garbage);
			freed += mb->objsize;
			mb->~mblock();
			delete[] reinterpret_cast<char *>(mb);
		}
		debug(freed << " bytes freed");

		busy = false;
		return freed;
	}

	// Garbage collection, mark phase.
	void basic_ptr::mark(basic_ptr *list)
	{ 
		for ( ; list ; list = list->next )
		{
			mblock *mb = list->mem;
			if ( mb && mb->active && !mb->marked )
			{
				mb->marked = true;
				mark(mb->members);
			}
		}
	}

	// Constructors, assignment operators and destructor.
	basic_ptr::basic_ptr() : mem(nullptr), pval(nullptr) { link(); }
	basic_ptr::basic_ptr(const basic_ptr &src) : mem(src.mem), pval(src.pval) { link(); }
	basic_ptr &basic_ptr::operator =(const basic_ptr &src)
	{
		mem = src.mem;
		pval = src.pval;
		return *this;
	}
	basic_ptr::basic_ptr(void *src) : mem(nullptr), pval(src) { link(); }
	basic_ptr &basic_ptr::operator =(void *src)
	{
		pval = src;
		return *this;
	}
	basic_ptr::basic_ptr(const basic_ptr &src, void *p) : mem(src.mem), pval(p) { link(); }
	basic_ptr::~basic_ptr() { unlink(); }
	
	// Check that this can be dereferenced.
	void basic_ptr::check() const
	{
		if ( !pval ) 
			throw ptr_exception("dereferencing null ptr"); 
		if ( mem && !mem->contains(pval) )
			throw ptr_exception("dereferencing out of bounds ptr"); 
	}

	// Begin allocation
	void *basic_ptr::alloc_begin(unsigned nelems, unsigned elem_size, destructor destr, bool zero)
	{
		// Eventually collect garbage
		gc(false);

		// Allocate memory block (header + objects)
		unsigned objsize = nelems * elem_size;
		try
		{
			mem = reinterpret_cast<mblock *>(new char[mblock::size() + objsize]);
		}
		catch (...)
		{
			mem = nullptr;
			throw;
		}

		// Initialize header and memory and push block on the construction stack
		new(mem) mblock(nelems, objsize, destr);
		char *obj = mem->obj();
		if ( zero )
			fill(obj, obj + objsize, 0);
		push(mem, constr_stack);

		return pval = obj;
	}

	// End allocation. 
	void basic_ptr::alloc_end(unsigned nconstructed)
	{ 
		pop(constr_stack);

		if ( !mem )							// Memory allocation failed
			return;

		if ( nconstructed < mem->nelems )	// A constructor threw
		{
			mem->nelems = nconstructed;
			mem->~mblock();
			delete[] reinterpret_cast<char *>(mem);
			mem = nullptr;
		}
		else
		{
			gc_m.lock();
			allocated += mem->objsize;
			gc_m.unlock();
			push(mem, new_blocks);
		}

		if ( constr_stack )					// Finished nested block
			return;
		
		// Finished bottom block, activate all new blocks
		active_m.lock();
		while ( new_blocks )
		{
			new_blocks->active = true;
			push(pop(new_blocks), active_blocks);
		}
		active_m.unlock();
	}

	// Insert this in the roots or members list
	inline void basic_ptr::link()
	{
		if ( constr_stack && constr_stack->contains(this) )	// A member
		{
//			debug("member " << this);
			next = constr_stack->members;
			constr_stack->members = prev = this;			// See unlink()
		}
		else												// A root
		{
//			debug("root " << this);
			prev = nullptr;
			roots_m.lock();
			if ( (next = roots) )
				roots->prev = this;
			roots = this;
			roots_m.unlock();
		}
	}

	// If this is a root, remove it from the roots list
	inline void basic_ptr::unlink()
	{
		if ( prev == this )		// A member, see link()
			return;

//		debug("root " << this);
		roots_m.lock();
		if ( next )
			next->prev = prev;
		if ( prev )
			prev->next = next;
		else
			roots = next;
		roots_m.unlock();
	}


	/////////////////////////
	// Class ptr_exception //
	/////////////////////////

	ptr_exception::ptr_exception(const char *s) : msg(s) { }
	const char *ptr_exception::what() { return msg; }

	////////////////////////////
	// Garbage collection API //
	////////////////////////////

	unsigned collect() { return basic_ptr::gc(true); }

	unsigned collect_threshold(unsigned newthr)
	{
		gc_m.lock();
		unsigned oldthr = threshold;
		if ( newthr )
			threshold = newthr; 
		gc_m.unlock();
		return oldthr;
	}
}
