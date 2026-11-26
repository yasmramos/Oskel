// Benchmark: append n integers to an array, then sum them. Same algorithm as
// `arrays.oskl`.
'use strict';

const n = 200000;
const xs = [];
for (let i = 0; i < n; i++) xs.push(i);

let sum = 0;
for (const x of xs) sum += x;
console.log(`arrays ${sum} ${xs.length}`);
