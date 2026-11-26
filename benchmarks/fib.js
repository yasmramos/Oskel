// Benchmark: naive recursive fibonacci. Same algorithm as `fib.oskl`.
'use strict';

function fib(n) {
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

console.log(`fib ${fib(30)}`);
