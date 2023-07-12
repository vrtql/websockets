#!/usr/bin/env bash

dir=${1:-"include"}

# For each file in $dir except those in .svn dir
for doc in $(find $dir -type f -prune -print)
do

  # The source file
  in_file=$doc

  # The converted html-ized file
  out_file=xml/include/$(basename $doc).html
  out_file_latex=xml/include/latex/$(basename $doc)

  # mtimes of each file
  in_file_mtime=$(stat -c "%Y" $in_file)

  # If the out_file exists
  if [ -f $out_file ]; then
    # Then stat it
    out_file_mtime=$(stat -c "%Y" $out_file)
  else
      # Otherwise it doesn't exist and we will just use zero, which will always
      # be less than the mtime of the input file, therefore forcing it to be
      # processed.
      out_file_mtime=0
  fi

  # If source file is newer than html-sized file
  if [ $in_file_mtime -gt $out_file_mtime ]; then

      # Regenerate the html-ized file
      echo "$doc -> $out_file"
      ret=$(emacs -batch -l scripts/.emacs -l scripts/code2html.el $doc 2> /dev/null | ./scripts/filter > $out_file )

      if [ $? = "1" ]; then
          # An error occurred. Print and exit
          printf "Error: in processing $doc. Description: $ret.\n"
          exit 1
      fi
  fi

done
