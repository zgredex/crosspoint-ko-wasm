#include <InflateBudget.h>

#include <cstdint>
#include <iostream>
#include <limits>

int main() {
  InflateBudget budget(10);
  if (!budget.charge(4) || budget.usedBytes() != 4 || budget.remainingBytes() != 6) return 1;
  if (!budget.charge(6) || budget.usedBytes() != 10 || budget.remainingBytes() != 0) return 2;

  // Rejections are transactional: a failed charge cannot wrap or mutate the
  // counter, even for the largest attacker-controlled declaration.
  if (budget.charge(1) || budget.usedBytes() != 10) return 3;
  if (budget.charge(std::numeric_limits<uint64_t>::max()) || budget.usedBytes() != 10) return 4;

  InflateBudget repeated(12);
  if (!repeated.charge(6) || !repeated.charge(6) || repeated.charge(6)) return 5;
  if (repeated.usedBytes() != 12) return 6;

  std::cout << "resource-budget: cumulative and overflow-safe charging verified\n";
  return 0;
}
