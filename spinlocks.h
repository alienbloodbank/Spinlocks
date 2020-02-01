#ifndef PHASE_H
#define PHASE_H

#include <bits/stdc++.h>

/* POSIX thread interface */
#include <pthread.h>
#include <sched.h>

/* Default values */
constexpr int THREAD_COUNT = 4;
constexpr int ITERATION_COUNT = 10000;

/* Using this to force a variable to take up complete cache line
   and decrease misses due to false sharing.
   Most common cache line size: 64 bytes 
   One way to know is to use 'cat /proc/cpuinfo | grep cache_alignment' to get
   cache lines sizes of all the CPUs on the system 
 */
#define CACHELINE_SIZE 64 

/* Counter location visible to all threads */
int global_counter;

/* Function to extract the vector of CPU sockets
 * Each socket is a group of cores
 * It reads the output of lscpu into a buffer and extracts the socket information
 * for #sockets and mappings of sockets to cores
 */
static void get_CPU_topology(std::vector<cpu_set_t>& cpu_sockets) {
	char buffer[4000];
	char *ext = buffer + 3000;
	char arch[10];
	cpu_set_t cpuset;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("lscpu", "r"), pclose);
	if (!pipe) {
	 std::cerr << "popen() failed!";
	 exit(EXIT_FAILURE);
	}
	int num_read = fread(buffer, sizeof(char), 2999, pipe.get());
	buffer[num_read] = '\0';
	char *reader = buffer;
	reader = strstr(reader, "Architecture:");
        reader += strlen("Architecture:");
	sscanf(reader, "%s", arch);
	std::cout << "Architecture: " << arch << std::endl;
	/* Checking affinity only on x86 */
	if(strcmp(arch, "x86_64") && strcmp(arch, "x86"))return;
	reader = strstr(reader, "Socket(s):");
	reader += strlen("Socket(s):");
	int num_sockets;
	sscanf(reader, "%d", &num_sockets);
	for (int i = 0; i < num_sockets; i++) {
		sprintf(ext, "NUMA node%d CPU(s):", i);
		reader = strstr(reader, ext);
		reader += strlen(ext);
		sscanf(reader, "%[^\n]s", ext);
		char *cpu = strtok(ext, ",\n\t");
		CPU_ZERO(&cpuset);
		while (cpu) {
			CPU_SET(atoi(cpu), &cpuset);
			cpu = strtok(NULL, ",\n\t");
		}
		cpu_sockets.push_back(cpuset);
	}
}

 /* High resolution timer class for measuring phase times */
class Timer
{
private:
	using hr_clock_t = std::chrono::high_resolution_clock;
	using millis_t = std::chrono::duration<double, std::milli>;

	std::chrono::time_point<hr_clock_t> m_beg;

public:
	Timer() : m_beg(hr_clock_t::now()) {}

	double elapsed() const {
		return std::chrono::duration_cast<millis_t>(hr_clock_t::now() - m_beg).count();
	}
};

/* Nmespace containing the implementation for the different spinlocks */
namespace locks458
{
	/* Test And Set Lock */
	class TASLock
	{
	private:
		alignas(CACHELINE_SIZE) std::atomic_flag f = ATOMIC_FLAG_INIT;
		/* Tuning parameters for backoff strategy */
		const int base = 10;
		const int limit = 10000;
		const int multiplier = 3;
	public:	
		void acquire() {
			while (std::atomic_flag_test_and_set_explicit(&f, std::memory_order_relaxed));
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void acquire_backoff() {
			int delay = base;
			while (std::atomic_flag_test_and_set_explicit(&f, std::memory_order_relaxed)) {
				/* Exponential Backoff delay */
				for(int i = 0;i < delay;i++);
				if (delay * multiplier <= limit) {
					delay *= multiplier;
				}
			}
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			std::atomic_flag_clear_explicit(&f, std::memory_order_release);
		}
	};

	/* The Ticket Lock */
	class TicketLock
	{
	private:
		alignas(CACHELINE_SIZE) std::atomic_int next_ticket;
		alignas(CACHELINE_SIZE) std::atomic_int now_serving;
		const int base = 10;
	public:
		TicketLock() {
			reset();
		}
		void acquire() {
			int my_ticket = std::atomic_fetch_add_explicit(&next_ticket, 1, std::memory_order_relaxed);
			while (true) {
				int ns = now_serving.load(std::memory_order_relaxed);
				if (ns == my_ticket) {
					break;
				}
			}
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void reset() {
			next_ticket = 0;
			now_serving = 0;
		}

		void acquire_backoff() {
			int my_ticket = std::atomic_fetch_add_explicit(&next_ticket, 1, std::memory_order_relaxed);
			do {
				int ns = now_serving.load(std::memory_order_relaxed);
				if (ns == my_ticket)
					break;
				/* Proportional Backoff delay */
				int delay = base * (my_ticket - ns);
				while(delay--);
			} while (true);
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			int t = now_serving.load(std::memory_order_relaxed) + 1;
			now_serving.store(t, std::memory_order_release);
		}
	};

	/* The MCS list-based queue lock */
	class MCSLock
	{
	public:
		struct qnode {
			alignas(CACHELINE_SIZE) std::atomic_bool waiting;
			alignas(CACHELINE_SIZE) std::atomic<qnode*> next;
		};
	private:
		alignas(CACHELINE_SIZE) std::atomic<qnode*> tail;
	public:
		MCSLock() {
			tail = nullptr;
		}
		void acquire(qnode *p) {
			p->next.store(nullptr, std::memory_order_relaxed);
			p->waiting.store(true, std::memory_order_relaxed);
			qnode *prev = std::atomic_exchange_explicit(&tail, p, std::memory_order_release);
			if (prev != nullptr) {
				prev->next.store(p, std::memory_order_relaxed);
				while (p->waiting.load(std::memory_order_relaxed));
			}
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release(qnode *p) {
			std::atomic_thread_fence(std::memory_order_release);
			qnode *succ = p->next.load(std::memory_order_relaxed);
			if (succ == nullptr) {
				qnode *_t = p;
				/* No spurious fail on x86 */
				if (std::atomic_compare_exchange_weak_explicit(&tail,
					&_t,
					succ,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
					return;
				do {
					succ = p->next.load(std::memory_order_relaxed);
				} while (succ == nullptr);
			}
			succ->waiting.store(false, std::memory_order_relaxed);
		}
	};

	/* The K42 variant of the MCS Lock */
	class K42_MCSLock
	{
	public:
		struct qnode {
			alignas(CACHELINE_SIZE) std::atomic<qnode*> tail;
			alignas(CACHELINE_SIZE) std::atomic<qnode*> next;
		};
	private:
		alignas(CACHELINE_SIZE) qnode q;
		/* Using this for serving as the waiting flag */
		alignas(CACHELINE_SIZE) qnode* const waiting = (qnode*)1;
	public:
		K42_MCSLock() {
			q.tail = nullptr;
			q.next = nullptr;
		}
		void acquire() {
			while (true) {
				qnode *prev = q.tail.load(std::memory_order_relaxed);
				if (prev == nullptr) {
					if (std::atomic_compare_exchange_weak_explicit(&q.tail,
						&prev,
						&q,
						std::memory_order_relaxed,
						std::memory_order_relaxed))
						break;
				}
				else {
					qnode n;
					n.tail = waiting;
					n.next = nullptr;
					qnode *_t = prev;
					std::atomic_thread_fence(std::memory_order_release);
					if (std::atomic_compare_exchange_weak_explicit(&q.tail,
						&_t,
						&n,
						std::memory_order_relaxed,
						std::memory_order_relaxed)) {
						prev->next.store(&n, std::memory_order_relaxed);
						while (n.tail.load(std::memory_order_relaxed) == waiting);
						/* Now we have the lock */
						qnode *succ = n.next.load(std::memory_order_relaxed);
						if (succ == nullptr) {
							q.next.store(nullptr, std::memory_order_relaxed);
							qnode *_t1 = &n;
							if (!std::atomic_compare_exchange_weak_explicit(&q.tail,
								&_t1,
								&q,
								std::memory_order_relaxed,
								std::memory_order_relaxed)) {
								do {
									succ = n.next.load(std::memory_order_relaxed);
								} while (succ == nullptr);
								q.next.store(succ, std::memory_order_relaxed);
							}
							break;
						}
						else {
							q.next.store(succ, std::memory_order_relaxed);
							break;
						}
					}
				}
			}
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			std::atomic_thread_fence(std::memory_order_release);
			qnode *succ = q.next.load(std::memory_order_relaxed);
			if (succ == nullptr) {
				qnode *e = &q;
				/* No spurious fail on x86 */
				if (std::atomic_compare_exchange_weak_explicit(&q.tail,
					&e,
					succ,
					std::memory_order_relaxed,
					std::memory_order_relaxed))
					return;
				do {
					succ = q.next.load(std::memory_order_relaxed);
				} while (succ == nullptr);
			}
			succ->tail.store(nullptr, std::memory_order_relaxed);
		}
	};

	/* The CLH list-based queue lock */
	class CLHLock
	{
	public:
		struct qnode {
			alignas(CACHELINE_SIZE) std::atomic<qnode*> prev;
			alignas(CACHELINE_SIZE) std::atomic_bool succ_must_wait;
		};
	private:
		alignas(CACHELINE_SIZE) std::atomic<qnode*> tail;
	public:
		CLHLock() {
			alignas(CACHELINE_SIZE) qnode *dummy = new qnode;
			dummy->prev = nullptr;
			dummy->succ_must_wait = false;
			tail = dummy;
		}
		void acquire(qnode *p) {
			p->succ_must_wait.store(true, std::memory_order_relaxed);
			qnode *pred = std::atomic_exchange_explicit(&tail, p, std::memory_order_release);
			p->prev.store(pred, std::memory_order_relaxed);
			while (pred->succ_must_wait.load(std::memory_order_relaxed));
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release(qnode **pp) {
			qnode* pred = (*pp)->prev;
			(*pp)->succ_must_wait.store(false, std::memory_order_release);
			*pp = pred;
		}
	};

	/* The K42 version of the CLH Lock */
	class K42_CLHLock
	{
	public:
		struct qnode {
			alignas(CACHELINE_SIZE) std::atomic_bool succ_must_wait;
		};
	private:
		pthread_key_t key;
		int m_num_threads;
		qnode *initial_thread_qnodes;
		alignas(CACHELINE_SIZE) qnode **thread_qnode_ptrs;
		alignas(CACHELINE_SIZE) qnode dummy;
		alignas(CACHELINE_SIZE) std::atomic<qnode*> tail;
		alignas(CACHELINE_SIZE) std::atomic<qnode*> head;
	public:
		K42_CLHLock(int num_threads) : m_num_threads(num_threads) {
			if (pthread_key_create(&key, nullptr) < 0) {
				std::cerr << "pthread_key_create failed in k42 CLH, errno=" << errno << std::endl;
				exit(EXIT_FAILURE);
			}
			dummy.succ_must_wait = false;
			tail = &dummy;
			initial_thread_qnodes = new qnode[num_threads];
			thread_qnode_ptrs = new qnode*[num_threads];
			for (int i = 0; i < num_threads; i++) {
				thread_qnode_ptrs[i] = initial_thread_qnodes + i;
			}
		}

		~K42_CLHLock() {
			pthread_key_delete(key);
			delete[] thread_qnode_ptrs, initial_thread_qnodes;
		}

		void set_threadid(int i) {
			pthread_setspecific(key, thread_qnode_ptrs + i);	
		}
	
		void acquire() {
			void *value = pthread_getspecific(key);
			int self = (static_cast<qnode**>(value) - thread_qnode_ptrs);
			qnode *p = thread_qnode_ptrs[self];
			p->succ_must_wait.store(true, std::memory_order_relaxed);
			qnode *pred = std::atomic_exchange_explicit(&tail, p, std::memory_order_release);
			while (pred->succ_must_wait.load(std::memory_order_relaxed));
			head.store(p, std::memory_order_relaxed);
			thread_qnode_ptrs[self] = pred;
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			qnode *_t = head.load(std::memory_order_relaxed);
			_t->succ_must_wait.store(false, std::memory_order_release);
		}
	};

	/* Lamport's Bakery Algorithm */
	class Lamport_Bakery
	{
	private:
		int m_num_threads;
		pthread_key_t key;
		alignas(CACHELINE_SIZE) std::atomic_int *number;
		alignas(CACHELINE_SIZE) std::atomic_bool *choosing;
	public:
		Lamport_Bakery(int num_threads) : m_num_threads(num_threads) {
			if (pthread_key_create(&key, nullptr) < 0) {
				std::cerr << "pthread_key_create failed in k42 CLH, errno=" << errno << std::endl;
				exit(EXIT_FAILURE);
			}
			choosing = new std::atomic_bool[num_threads];
			number = new std::atomic_int[num_threads];
			for(int i = 0;i< m_num_threads;i++){
				number[i] = 0;
				choosing[i] = false;
			}
		}

		~Lamport_Bakery() {
			pthread_key_delete(key);
			delete[] choosing, number;
		}

		void set_threadid(int i) {
			pthread_setspecific(key, number + i);	
		}
		
		void acquire() {
			void *value = pthread_getspecific(key);
			int self = (static_cast<std::atomic_int*>(value) - number);
			
			choosing[self].store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);	
			
			int m = 0;
			for(int i = 0;i < m_num_threads;i++) {
				if(number[i].load(std::memory_order_relaxed) > m)
					m = number[i].load(std::memory_order_relaxed);
			}
				
			number[self].store(++m, std::memory_order_release);
			
			choosing[self].store(false, std::memory_order_relaxed);
			int t;
			for(int i = 0;i < m_num_threads;i++) {
				while (choosing[i].load(std::memory_order_relaxed));
				do {
					t = number[i].load(std::memory_order_relaxed);
				} while (t != 0 && (std::tie(t, i) < std::tie(m, self)));
			}
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			void *value = pthread_getspecific(key);
			int self = (static_cast<std::atomic_int*>(value) - number);
			number[self].store(0, std::memory_order_release);
		}
	};
	
	/* Anderson's array-based queue lock */
	class Anderson
	{
	private:
		/* Ensuring that the cache-coherent spin locations are not affected by false sharing */
	        using padded = std::pair<std::atomic_bool, uint8_t[CACHELINE_SIZE - sizeof(std::atomic_bool)]>;
		int m_num_threads;
		alignas(CACHELINE_SIZE) padded *slots;
		alignas(CACHELINE_SIZE) std::atomic_int next_slot;
	public:
		Anderson(int num_threads) : m_num_threads(num_threads) {
			slots = new padded[num_threads];
			next_slot = 0;
			for(int i = 1;i < num_threads;i++) {
				slots[i].first = false;
			}
			slots[0].first = true;
		}

		~Anderson() {
			delete[] slots;
		}

		void acquire(int *my_place) {
			*my_place = std::atomic_fetch_add_explicit(&next_slot, 1, std::memory_order_relaxed);
			if (*my_place == m_num_threads - 1) {
				std::atomic_fetch_add_explicit(&next_slot, -m_num_threads, std::memory_order_relaxed);
			}
			std::atomic_thread_fence(std::memory_order_release);
			*my_place = *my_place % m_num_threads;
			while(slots[*my_place].first.load(std::memory_order_relaxed) == false);
			slots[*my_place].first.store(false, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release(int *my_place) {
			slots[(*my_place + 1) % m_num_threads].first.store(true, std::memory_order_release);
		}
	};
	
	/* Graunke and Thakkar's array-based queue lock 
	 * Note: Currently this is incomplete code
	 */
	/*class Graunke_Thakkar
	{
	private:
		std::atomic_int who_was_last;
		std::atomic_bool this_means_locked;
		int m_num_threads;
		pthread_key_t key;
		std::atomic_bool *slots;
	public:
		Graunke_Thakkar(int num_threads) : m_num_threads(num_threads) {
			if (pthread_key_create(&key, nullptr) < 0) {
				std::cerr << "pthread_key_create failed in k42 CLH, errno=" << errno << std::endl;
				exit(EXIT_FAILURE);
			}
			slots = new std::atomic_bool[num_threads];
			who_was_last = 0;
			this_means_locked = false;
			for(int i = 0;i < m_num_threads;i++)slots[i] = true;		
		}

		~Graunke_Thakkar() {
			pthread_key_delete(key);
			delete[] slots;
		}
		
		void set_threadid(int i) {
			pthread_setspecific(key, slots + i);	
		}
		
		void acquire() {
			void *value = pthread_getspecific(key);
			int self = (static_cast<std::atomic_bool*>(value) - slots);
						
			std::atomic_thread_fence(std::memory_order_release);
			int who_is_ahead_of_me = std::atomic_exchange_explicit(&who_was_last, 
				self, std::memory_order_relaxed);
			int what_is_locked = std::atomic_exchange_explicit(&this_means_locked, 
				slots[self].load(std::memory_order_relaxed), std::memory_order_relaxed);
			
			while(slots[who_is_ahead_of_me].load(std::memory_order_release) == what_is_locked);
			
			std::atomic_thread_fence(std::memory_order_acq_rel);
		}

		void release() {
			void *value = pthread_getspecific(key);
			int self = (static_cast<std::atomic_bool*>(value) - slots);
			bool _t = slots[self].load(std::memory_order_relaxed);
			slots[self].store(!_t, std::memory_order_release);
		}
	};*/
}

/* Class for running experiments on each phase */
class LockTest
{
public:
	enum LockType
	{
		CPP_MUTEX,
		NAIVE_TAS,
		BACKOFF_TAS,
		NAIVE_TICKET,
		BACKOFF_TICKET,
		MCS,
		K42_MCS,
		CLH,
		K42_CLH,
		LAMPORT_BAKERY,
		ANDERSON,
		GRAUNKE_THAKKAR
	};
private:
	const int m_num_threads = THREAD_COUNT;
	const int m_num_iterations = ITERATION_COUNT;
	std::vector<std::thread> threads;
	std::vector<cpu_set_t> m_cpu_sockets;
	cpu_set_t full_set;

	std::mutex mutex_lock;
		
	/* Declare all our custom spinlocks */
	locks458::TASLock tas_lock;
	locks458::TicketLock ticket_lock;
	locks458::MCSLock mcs_lock;
	locks458::K42_MCSLock k42_mcs_lock;
	locks458::CLHLock clh_lock;
	locks458::K42_CLHLock k42_clh_lock;
	locks458::Lamport_Bakery lb_lock;
	locks458::Anderson a_lock;
	//locks458::Graunke_Thakkar gt_lock;

	int affinity_split_count = 0;
	double elapsed_time = 0.0;
public:
	LockTest(int num_threads, int num_iterations) : m_num_threads(num_threads), m_num_iterations(num_iterations), 
		k42_clh_lock(num_threads), lb_lock(num_threads), a_lock(num_threads) {
	
		/* Getting mappings of sockets to CPUs */
		get_CPU_topology(m_cpu_sockets);

		/* Create a default set of all the CPUs in the system */
		CPU_ZERO(&full_set);
		int num_cpus = std::thread::hardware_concurrency();
		
		std::cout << "Hardware threads on the machine: " << num_cpus << std::endl;

		for (int i = 0; i < num_cpus; i++) {
			CPU_SET(i, &full_set);
		}
		/*
		for (int i = 0;i < num_cpus;i++) {
			if(CPU_ISSET(i, &full_set)) {
				std::cout << i << std::endl;
			}
		}*/
	}
	
	~LockTest(){}

	/* Set the number of equal thread splits required for restricting set of threads to set of sockets */
	void split_CPU_affinity(int asplit_count);
	/* Execute a phase */
	void run(LockType phase_type);
	friend std::ostream& operator<< (std::ostream &out, const LockTest& phase);
};

std::ostream& operator<< (std::ostream &out, const LockTest& phase)
{
	out << "Counter Value: " << global_counter << "\nTime taken (milliseconds): " << phase.elapsed_time <<
		"\nThroughput (Iterations per millisecond): " <<
		(phase.m_num_threads * phase.m_num_iterations) / phase.elapsed_time;
	return out;
}

void LockTest::split_CPU_affinity(int asplit_count) {
	/* If there aren't enough threads and sockets,
	 * then revert back to thread scheduling on all CPUs
	 */
	if (m_num_threads < asplit_count || asplit_count > m_cpu_sockets.size()) {
		affinity_split_count = 0;
		std::cerr << "Splitting affinity failed!" << std::endl;
		return;
	}
	affinity_split_count = asplit_count;
}

void LockTest::run(LockType phase_type) {
	/* Flag to force all the threads to start computations at the same time */
	std::atomic_bool start(false);

	/* Resetting counter */
	global_counter = 0;

	ticket_lock.reset();
	/* Launching threads */
	for (int i = 0; i < m_num_threads; i++) {
		/* Creating in-place threads as these objects are not CopyConstructible */
		threads.emplace_back([&, i]()
		{
			int num_iterations = m_num_iterations;
			
			alignas(CACHELINE_SIZE) locks458::MCSLock::qnode mcs_qnode;
			alignas(CACHELINE_SIZE) locks458::CLHLock::qnode *clh_qnode;
			alignas(CACHELINE_SIZE) int my_place;
			
			/* Set thread ids for referencing thread specific data structures */
			if (phase_type == K42_CLH) {
				k42_clh_lock.set_threadid(i);
			}
			if (phase_type == LAMPORT_BAKERY) {
				lb_lock.set_threadid(i);
			}/*
			if (phase_type == GRAUNKE_THAKKAR) {
				gt_lock.set_threadid(i);
			}*/
			
			/* Spinning */
			while (!start.load());

			/* Running respective phase lock */
			switch (phase_type) {
			case CPP_MUTEX:
				while (num_iterations--) {
					std::lock_guard<std::mutex> locker(mutex_lock);
					::global_counter++;
				}
				break;
			case NAIVE_TAS:
				while (num_iterations--) {
					tas_lock.acquire();
					::global_counter++;
					tas_lock.release();
				}
				break;
			case BACKOFF_TAS:
				while (num_iterations--) {
					tas_lock.acquire_backoff();
					::global_counter++;
					tas_lock.release();
				}
				break;
			case NAIVE_TICKET:
				while (num_iterations--) {
					ticket_lock.acquire();
					::global_counter++;
					ticket_lock.release();
				}
				break;
			case BACKOFF_TICKET:
				while (num_iterations--) {
					ticket_lock.acquire_backoff();
					::global_counter++;
					ticket_lock.release();
				}
				break;
			case MCS:
				while (num_iterations--) {
					mcs_lock.acquire(&mcs_qnode);
					::global_counter++;
					mcs_lock.release(&mcs_qnode);
				}
				break;
			case K42_MCS:
				while (num_iterations--) {
					k42_mcs_lock.acquire();
					::global_counter++;
					k42_mcs_lock.release();
				}
				break;
			case CLH:
				while (num_iterations--) {
					clh_qnode = new locks458::CLHLock::qnode;
					clh_lock.acquire(clh_qnode);
					::global_counter++;
					clh_lock.release(&clh_qnode);
					/* node is now pointing to the predecessor */
					delete clh_qnode;
				}
				break;
			case K42_CLH:
				while (num_iterations--) {
					k42_clh_lock.acquire();
					::global_counter++;
					k42_clh_lock.release();
				}
				break;
			case LAMPORT_BAKERY:
				while (num_iterations--) {
					lb_lock.acquire();
					::global_counter++;
					lb_lock.release();
				}
				break;
			case ANDERSON:
				while (num_iterations--) {
					a_lock.acquire(&my_place);
					::global_counter++;
					a_lock.release(&my_place);
				}
				break;
			/*case GRAUNKE_THAKKAR:
				while (num_iterations--) {
					gt_lock.acquire();
					::global_counter++;
					gt_lock.release();
				}
				break;*/
			default:
				break;
			}
		});

		/* Schedule thread on socket(s) based on the split count */
		if (affinity_split_count) {
			if (pthread_setaffinity_np(threads.back().native_handle(),
				sizeof(cpu_set_t), &m_cpu_sockets[i / ((m_num_threads + 1) / affinity_split_count)])) {
				std::cerr << "Error calling pthread_setaffinity_np for thread# :" << i << std::endl;
			}
		}
		else {
			if (pthread_setaffinity_np(threads.back().native_handle(), sizeof(cpu_set_t), &full_set)) {
				std::cerr << "Error calling pthread_setaffinity_np for thread# :" << i << std::endl;
			}
		}
	}

	/* Starting high resolution timer */
	Timer phase_time;

	/* Phase Start */
	start.store(true);

	/* Main thread waiting for all other threads to finish */
	for (auto& t : threads) {
		if (t.joinable()) {
			t.join();
		}
	}

	/* Phase End */

	/* Getting execution time for the phase */
	elapsed_time = phase_time.elapsed();
}

#endif
