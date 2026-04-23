<?php

function p($s)
{
    echo $s . "\n";
}

$x = 5;
$y = 3;
$z = 5;

if ($x != $y) {
    p("5 != 3: ok");
}

if ($x === $z) {
    p("5 === 5: ok");
}

if ($x !== $y) {
    p("5 !== 3: ok");
}

for ($i = 0; $i <= 4; $i++) {
    if ($i > 2) {
        p("i > 2: ok");
    }
}

for ($i = 10; $i >= 8; $i = $i + -1) {
    p("i >= 8: ok");
}
