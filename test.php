<?php

function p($s)
{
    echo $s . "\n";
}

for ($x = 0; $x < 10; $x++) {
    if ($x % 2 == 0) {
        p("Hello world $x!");
    }
}
