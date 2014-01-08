#include <stdio.h>
#include <thread>
#include "gcptr.h"

using namespace std;
using namespace gcptr;

// Circularly referencing classes: A -> B -> C -> A
// Constructor of A makes a smart pointer to itself and allocates a B
// object which receives this pointer.
// Constructor of B allocates a C object which receives the original A
// pointer and stores it.

struct A;
struct B;
struct C;

struct A
{
	A(); ~A();
	ptr<B> p;
};

struct B
{
	B(ptr<A> root); ~B();
	ptr<C> p;
};

struct C
{
	C(ptr<A> root); ~C();
	ptr<A> p;
};

A::A()
{ 
	puts("const A"); 
	ptr<A> a(this); 
	a.attach(); 
	p.alloc(a); 
}

A::~A() { printf("dest A %p\n", this); }

B::B(ptr<A> root) 
{ 
	puts("const B"); 
	p.alloc(root); 
}

B::~B() { printf("dest B %p\n", this); }

C::C(ptr<A> root): p(root) { puts("const C"); }

C::~C() { printf("dest C %p\n", this); }

void body()
{
	try
	{
		// Test some basic functionality
		unsigned i;
		const int dim = 4;
		ptr<int> pi;
		pi.alloc_array(dim, init_zero);
		ptr<int> iter;
		puts("initial values");
		for (iter = pi ; iter < pi + dim ; ++iter)
			printf("%d\n", *iter);
		for (i = 0, iter = pi ; iter < pi + dim ; ++iter)
			*iter = ++i;
		puts("final values");
		for (iter = pi ; iter < pi + dim ; ++iter)
			printf("%d\n", *iter);
		pi.detach();
		puts("detach pi");
		collect();	// iter still holds a reference to the array
		iter.detach();
		puts("detach iter");
		collect();	// No references remain, array should be deleted here

		// Create an array of 3 objects of type A, this creates 3 A->B->C->A cycles
		ptr<A> pa;
		pa.alloc_array(3);

		// Create pointers to member "p" of the three C objects in three 
		// different ways.
		ptr<ptr<A>> ppa0(pa[0].p->p, &C::p);
		ptr<ptr<A>> ppa1(pa[1].p->p, &pa[1].p->p->p);
		ptr<ptr<A>> ppa2 = &pa[2].p->p->p;
		ppa2.attach(pa[2].p->p);

		puts("all attached");
		collect();				// 4 references to the array are active
		pa.detach();
		puts("detach pa");
		collect();				// 3 references to the array are active
		ppa0.detach();
		puts("detach ppa0");
		collect();				// 2 references to the array are active
		ppa1.detach();
		puts("detach ppa1");
		collect();				// 1 reference to the array is active
		ppa2.detach();
		puts("detach ppa2");
		collect();				// Array should be deleted here
	}
	catch (ptr_exception e)
	{
		puts(e.what());
	}
}

int main(int argc, char *argv[])
{
	// argv[1] is number of threads, default = 1
	unsigned nthr = 1;
	if ( argc > 1 )
		nthr = atoi(argv[1]);
	if ( !nthr )
		nthr = 1;

	// Run and join threads
	thread th[nthr];
	for ( unsigned i = 0 ; i < nthr ; i++ )
		th[i] = thread(body);
	for ( unsigned i = 0 ; i < nthr ; i++ )
		th[i].join();

	return 0;
}
