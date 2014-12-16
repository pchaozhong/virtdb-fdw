#!/usr/bin/perl

use strict;

my $prev    = 0;
my $result  = "v0.0.0";

while( <> )
{
	if( /^([\w\d]+)\trefs\/tags\/v(\d+)\.(\d+)\.(\d+)/ )
	{
		my $v      = $1;
		my $maj    = $2;
		my $min    = $3;
		my $build  = $4;
		my $tmp    = (0+$maj)*100000000 + (0+$min)*100000 + $build;
                if( $tmp > $prev )
		{
			my $next_build = 1+$build;
			$prev    = $tmp;
			$result  = "v${maj}.${min}.${next_build}";
		}
	}
}

print "$result\n"
