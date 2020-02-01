/* CSC 2/458: Parallel and Distributed Systems
 * Spring 2019
 * Assignment 3: Spin Locks
 * Author: Soubhik Ghosh (netId: sghosh13)
 */

#include "spinlocks.h"

 /* List of phase locks */
const std::map<LockTest::LockType, std::string> phase_types = {
	{ LockTest::CPP_MUTEX, "C++ mutex"},
	{ LockTest::NAIVE_TAS, "Naive TAS lock"},
	{ LockTest::BACKOFF_TAS, "TAS with exponential backoff"},
	{ LockTest::NAIVE_TICKET, "Naive ticket lock"},
	{ LockTest::BACKOFF_TICKET, "Ticket lock with proportional backoff"},
	{ LockTest::MCS, "MCS lock"},
	{ LockTest::K42_MCS, "K42 MCS lock"},
	{ LockTest::CLH, "CLH lock"},
	{ LockTest::K42_CLH, "K42 CLH lock"},
	{ LockTest::LAMPORT_BAKERY, "Lamport's bakery algorithm"},
	{ LockTest::ANDERSON, "Anderson's array-based queuing lock"},
//	{ LockTest::GRAUNKE_THAKKAR, "Graunke and Thakkar's array-based queuing lock"}	
};

/* Function to extract number of threads and iterations from command line arguments */
std::tuple<int, int> get_cmd_line_args(int argc, char **argv)
{
	enum class Args
	{
		THREAD_FLAG,
		ITERATION_FLAG,
		NONE
	};

	int num_threads = THREAD_COUNT, num_iterations = ITERATION_COUNT;
	Args flag = Args::NONE;

	for (int i = 1; i < argc; i++) {
		std::string s(argv[i]);
		if (s.compare("-t") == 0)
			flag = Args::THREAD_FLAG;
		else if (s.compare("-i") == 0)
			flag = Args::ITERATION_FLAG;
		else {
			if (flag != Args::NONE && s.find_first_not_of("0123456789") == std::string::npos) {
				[&]() -> int& {
					if (flag == Args::THREAD_FLAG)
						return num_threads;
					if (flag == Args::ITERATION_FLAG)
						return num_iterations;
					return num_threads;
				} () = std::stoi(s);
			}
			flag = Args::NONE;
		}
	}
	return std::make_tuple(num_threads, num_iterations);
}

int main(int argc, char *argv[])
{
	int num_threads;
	int num_iterations;
	/* Getting number of threads, iterations from the command line */
	std::tie(num_threads, num_iterations) = get_cmd_line_args(argc, argv);

	/* Initialize experiments */
	LockTest experiment(num_threads, num_iterations);

	std::cout << "\n" << std::flush;
		
	std::cout << "Thread count: " << num_threads << std::endl;
	std::cout << "Iteration count: " << num_iterations << std::endl;

	std::cout << "\n" << std::flush;

	/* Use this function to pin threads to sockets
	 * 0: Threads can run on any CPU (default)
	 * 1: Pin threads to the first socket
	 * 2: Split the threads 50/50 between 2 sockets
	 * .
	 * N: Split th threads equally between N sockets
	 */
	experiment.split_CPU_affinity(0);
	
	std::cout << "Experiments:" << std::endl;
	for (auto const& p : phase_types) {
		experiment.run(p.first);
		std::cout << p.second << "\n" << experiment << "\n\n";
	}

	std::cout << "\n" << std::flush;

	return EXIT_SUCCESS;
}
