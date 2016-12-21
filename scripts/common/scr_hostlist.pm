package scr_hostlist;
use strict;

# This package processes slurm-style hostlist strings.
#
# expand($hostlist)
#   returns a list of individual hostnames given a hostlist string
# compress(@hostlist)
#   returns an ordered hostlist string given a list of hostnames
#
# Author:  Adam Moody (moody20@llnl.gov)

# Returns a list of hostnames, give a hostlist string
# expand("rhea[2-4,6]") returns ('rhea2','rhea3','rhea4','rhea6')
sub expand {
  # read in our hostlist, should be first parameter
  if (@_ != 1) {
    return undef;
  }
  my $nodeset = shift @_;

  my @nodes = ();

  # split entries on commas
  # machine[1-3,5],machine[7-8],machine10
  my @chunks = split ",", $nodeset;
  while (@chunks > 0) {
    # look for opening bracket
    my $chunk = shift @chunks;
    if ($chunk =~ /^(.*)\[(.*)$/) {
      # got a starting bracket, scan until we find the closing bracket
      my $prefix  = $1;
      my $content = $2;
      my $suffix  = "";

      # build a list of ranges until we find the closing bracket
      my @ranges = ();
      if ($content =~ /^(.*)\](.*)$/) {
        # found the closing bracket (and optional suffix) in the same chunk
        push @ranges, $1;
        $suffix = $2;
      } else {
        # no bracket, so we either got a single number or a range here
        push @ranges, $content;

        # pluck off entries until we find the closing bracket
        while ($chunks[0] !~ /^(.*)\](.*)$/) {
          $chunk = shift @chunks;
          push @ranges, $chunk;
        }

        # if well formed, this item must now have the bracket
        $chunk = shift @chunks;
        if ($chunk =~ /^(.*)\](.*)$/) {
          # found the closing bracket (and optional suffix)
          push @ranges, $1;
          $suffix = $2;
        }
      }

      # expand ranges to pairs of low/high values
      my @lowhighs = ();
      my $numberLength = 0; # for leading zeros, e.g atlas[0001-0003]
      foreach my $range (@ranges) {
        my $low  = undef;
        my $high = undef;
        if ($range =~ /(\d+)-(\d+)/) {
          # low-to-high range
          $low  = $1;
          $high = $2;
        } else {
          # single element range
          $low  = $range;
          $high = $range;
        }
        #if the lowest number starts with 0
        if($numberLength == 0 and index($low,"0") == 0){ 
           $numberLength = length($low);
        }
        push @lowhighs, $low, $high;
      }

      # produce our list of node names
      while(@lowhighs) {
        my $low  = shift @lowhighs;
        my $high = shift @lowhighs;
        for(my $i = $low; $i <= $high; $i++) {
          # tack on leading 0's if input had them
          my $nodenumber = sprintf("%0*d", $numberLength, $i);
          my $nodename = $prefix . $nodenumber . $suffix;
          push @nodes, $nodename;
        }
      }
    } else {
      # no brackets, just a single node name, copy it verbatim
      push @nodes, $chunk;
    }
  }

#  my $machine = undef;
#  my @lowhighs = ();
#  my $numberLength = 0; # for leading zeros, e.g atlas[0001-0003]
#  if ($nodeset =~ /([\D]*)\[([\d,-]+)\]/) {
#    # hostlist with brackets, e.g., atlas[2-5,28,30]
#    $machine = $1;
#    my @ranges = split ",", $2;
#    foreach my $range (@ranges) {
#      my $low  = undef;
#      my $high = undef;
#      if ($range =~ /(\d+)-(\d+)/) {
#        # low-to-high range
#        $low  = $1;
#        $high = $2;
#      } else {
#        # single element range
#        $low  = $range;
#        $high = $range;
#      }
#      #if the lowest number starts with 0
#      if($numberLength == 0 and index($low,"0") == 0){ 
#         $numberLength = length($low);
#      }
#      push @lowhighs, $low, $high;
#    }
#  } else {
#    # single node hostlist, e.g., atlas2
#    $nodeset =~ /([\D]*)(\d+)/;
#    $machine = $1;
#    $numberLength = length($2);
#    push @lowhighs, $2, $2;
#  }
#
#  # produce our list of nodes
#  my @nodes = ();
#  while(@lowhighs) {
#    my $low  = shift @lowhighs;
#    my $high = shift @lowhighs;
#    for(my $i = $low; $i <= $high; $i++) {
#      my $nodenumber = sprintf("%0*d", $numberLength, $i);
#      #print $nodenumber;
#      push @nodes, $machine . $nodenumber;
#    }
#  }

  return @nodes;
}

# Returns a hostlist string given a list of hostnames
# compress('rhea2','rhea3','rhea4','rhea6') returns "rhea[2-4,6]"
sub compress {
  if (@_ == 0) {
    return "";
  }

  # sort the node names and join them with commas
  my @nodes = sort {$a cmp $b} @_;
  return join(",", @nodes);
}

# Given references to two lists, subtract elements in list 2 from list 1 and return remainder
sub diff {
  # we should have two list references
  if (@_ != 2) {
    return undef;
  }
  my $set1 = $_[0];
  my $set2 = $_[1];

  my %nodes = ();

  # build list of nodes from set 1
  foreach my $node (@$set1) {
    $nodes{$node} = 1;
  }

  # remove nodes from set 2
  foreach my $node (@$set2) {
    delete $nodes{$node};
  }

  my @nodelist = (keys %nodes);
  if (@nodelist > 0) {
    my $list = scr_hostlist::compress(@nodelist);
    return scr_hostlist::expand($list);
  }
  return ();
}

# Given references to two lists, return list of intersection nodes
sub intersect {
  # we should have two list references
  if (@_ != 2) {
    return undef;
  }
  my $set1 = $_[0];
  my $set2 = $_[1];

  my %nodes = ();

  # build list of nodes from set 1
  my %tmp_nodes = ();
  foreach my $node (@$set1) {
    $tmp_nodes{$node} = 1;
  }

  # remove nodes from set 2
  foreach my $node (@$set2) {
    if (defined $tmp_nodes{$node}) {
      $nodes{$node} = 1;
    }
  }

  my @nodelist = (keys %nodes);
  if (@nodelist > 0) {
    my $list = scr_hostlist::compress(@nodelist);
    return scr_hostlist::expand($list);
  }
  return ();
}

1;
