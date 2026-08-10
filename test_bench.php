<?php

function p($s)
{
    echo $s . "\n";
}

for ($x = 0; $x < 1_000_000_000; $x++) {
    if ($x % 100_000_000 == 0) {
        p("Hello world $x!");
    }
}
