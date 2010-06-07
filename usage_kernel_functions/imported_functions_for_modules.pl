my %functions = ();
for my $module (@ARGV)
{
	my $module_path = `modprobe -l $module`;
	chomp $module_path;
	if($module_path eq "")
	{
		print("Cannot find module $module.\n");
		next;
	}
    #On some systems `modprobe -l` return relative path.
    #If it is your case, uncomment next line.
    #$module_path = "/lib/modules/`uname -r`/$module_path";
	my @functions_local = `perl imported_functions.pl $module_path`;
	for my $function (@functions_local)
	{
		chomp $function;
		if($functions{$function})
		{
			$functions{$function} = $functions{$function} + 1;
		}
		else
		{
			$functions{$function} = 1;
		}
	}
}
my @functions_sorted = sort {$functions{$b} <=> $functions{$a}} keys %functions;
for my $function (@functions_sorted)
{
	printf("%-30s %d\n", $function, $functions{$function});
}