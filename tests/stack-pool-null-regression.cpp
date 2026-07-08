/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <darlingserver/stack-pool.hpp>

#include <cstdio>

int main() {
	DarlingServer::StackPool pool(0, 64 * 1024, true);
	DarlingServer::StackPool::Stack empty;

	// Regression for dar-9f8b: the old free() path accepted an empty handle in
	// release builds, parked nullptr in the idle list, and the next allocate()
	// handed that nullptr to makecontext().
	pool.free(empty);

	DarlingServer::StackPool::Stack allocated;
	pool.allocate(allocated);
	if (!allocated.isValid() || allocated.base == nullptr) {
		std::fprintf(stderr, "StackPool returned an invalid stack after free(empty)\n");
		return 1;
	}

	pool.free(allocated);
	return 0;
}
