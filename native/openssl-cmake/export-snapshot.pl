#! /usr/bin/env perl
#
# HmRdp - OpenSSL 3.x "route A" snapshot exporter.
#
# OpenSSL has no CMake build system: its source tree ships only Perl template
# files (*.h.in) and build.info files, and `Configure` turns them into the
# generated headers and the exact source list a build compiles. On Windows there
# is no Perl, so the alternative to the WSL `perl Configure && make` step is to
# freeze that Configure output once and let CMake compile it afterwards.
#
# This script extracts the two things that Configure produces and that the CMake
# build cannot derive on its own:
#
#   1. the generated files (public headers, the provider DER shims, ...) that
#      `perl Configure` writes into the source tree, and
#   2. the exact object -> source mapping for libcrypto/libssl, expanded through
#      the providers archives (libdefault.a/liblegacy.a/libcommon.a are archived
#      into libcrypto.a by the generated Makefile, so their objects belong in the
#      libcrypto source list).
#
# It runs against an *already configured* tree (one that has configdata.pm and
# the generated files in place). See native/scripts/export-openssl-snapshot.ps1
# for the wrapper that produces such a tree with build-openssl-wsl.sh.
#
#   perl export-snapshot.pl <configured-tree> <out-dir> [arch]
#
# arch defaults to a name derived from the configured target (linux-aarch64 ->
# arm64-v8a, linux-x86_64 -> x86_64, linux-armv4 -> armeabi-v7a). Only
# include/openssl/configuration.h is architecture dependent for the 64-bit
# targets; everything else is written to the shared set.

use strict;
use warnings;
no warnings 'once';
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Copy qw(copy);

my ($tree, $out, $arch) = @ARGV;
die "usage: export-snapshot.pl <configured-tree> <out-dir> [arch]\n"
    if !defined $tree || !defined $out;

$tree =~ s{/+$}{};
$out  =~ s{/+$}{};

die "not a configured tree (no configdata.pm): $tree\n"
    if !-f "$tree/configdata.pm";

chdir $tree or die "chdir $tree: $!\n";
do "./configdata.pm" or die "load configdata: $@\n";

my $S      = $configdata::unified_info{sources};
my $GEN    = $configdata::unified_info{generate} || {};
my $target = $configdata::config{target} || '';
my %archmap = (
    'linux-aarch64' => 'arm64-v8a',
    'linux-x86_64'  => 'x86_64',
    'linux-armv4'   => 'armeabi-v7a',
);
$arch //= $archmap{$target};
die "cannot derive arch from target '$target'; pass it explicitly\n" if !$arch;

sub copy_out {
    my ($rel, $dst_root) = @_;
    my $src = "$tree/$rel";
    die "missing generated file: $src\n" if !-f $src;
    my $dst = "$dst_root/$rel";
    make_path(dirname($dst));
    copy($src, $dst) or die "copy $src -> $dst: $!\n";
}

# --- 1. generated files -------------------------------------------------------
# Everything Configure generates that lands under include/, crypto/ or
# providers/ as a .h/.c. Excluded:
#   include/openssl/configuration.h  -> written per-arch below (RC4_INT)
#   crypto/buildinf.h                -> embeds the build machine path/flags; the
#                                        CMake build writes a neutral one instead
#   libcrypto.ld / libssl.ld / *.rc  -> version scripts / Windows resources,
#                                        neither is used by a static build
my $common_root = "$out/generated";
my $count = 0;
for my $g (sort keys %$GEN) {
    next if $g !~ m{^(include/|crypto/|providers/).*\.(h|c)$};
    next if $g eq 'include/openssl/configuration.h';
    next if $g eq 'crypto/buildinf.h';
    next if $g =~ m{/test/};
    copy_out($g, $common_root);
    $count++;
}
my $arch_dst = "$out/generated/arch/$arch/configuration.h";
make_path(dirname($arch_dst));
copy("$tree/include/openssl/configuration.h", $arch_dst)
    or die "copy configuration.h: $!\n";
$count++;

# --- 2. source lists ----------------------------------------------------------
# Resolve a target to its object files, recursing through aggregated archives.
sub objs_of {
    my ($name, $stack) = @_;
    $stack ||= {};
    die "cycle resolving $name\n" if $stack->{$name}++;
    my $list = $S->{$name};
    die "no sources entry for '$name'\n" if !defined $list;
    my @res;
    for my $e (@$list) {
        if ($e =~ /\.a$/) {
            push @res, objs_of($e, $stack);
        } elsif ($e =~ /\.o$/) {
            push @res, $e;
        } else {
            die "unexpected source entry '$e' while resolving '$name'\n";
        }
    }
    delete $stack->{$name};
    return @res;
}

sub real_sources {
    my ($lib) = @_;
    my @objs = objs_of($lib);
    push @objs, objs_of('providers/libdefault.a');
    push @objs, objs_of('providers/liblegacy.a');
    push @objs, objs_of('providers/libcommon.a');
    my (%seen, @srcs);
    for my $o (@objs) {
        next if $seen{$o}++;
        my $l = $S->{$o};
        die "no source for object '$o'\n" if !defined $l;
        die "'$o' maps to " . scalar(@$l) . " sources\n" if @$l != 1;
        my $src = $l->[0];
        die "unsupported source '$src' for '$o'\n" if $src !~ /\.c$/;
        push @srcs, $src;
    }
    return @srcs;
}

# libssl does not aggregate the provider archives; objs_of on libssl is enough.
sub objs_of_plain {
    my ($name) = @_;
    my @objs = objs_of($name);
    my (%seen, @srcs);
    for my $o (@objs) {
        next if $seen{$o}++;
        my $l = $S->{$o};
        die "no source for object '$o'\n" if !defined $l;
        my $src = $l->[0];
        push @srcs, $src;
    }
    return @srcs;
}

my @crypto = real_sources('libcrypto');
my @ssl    = objs_of_plain('libssl');

# A source that Configure generated (*_gen.c) only exists under the snapshot;
# emit it against ${OSSL_OVERLAY}, everything else against ${OSSL_SRC}.
sub emit {
    my ($fh, $src) = @_;
    my $base = ($GEN->{$src} ? '${OSSL_OVERLAY}' : '${OSSL_SRC}');
    print $fh "    $base/$src\n";
}

make_path("$out/generated");
open(my $fh, '>', "$out/generated/sources.cmake") or die "write sources.cmake: $!\n";
print $fh "# Generated by native/openssl-cmake/export-snapshot.pl - do not edit by hand.\n";
print $fh "# Re-run native/scripts/export-openssl-snapshot.ps1 after changing the OpenSSL version.\n";
print $fh "# Paths are prefixed with \${OSSL_SRC} (fetched upstream tree) or \${OSSL_OVERLAY}\n";
print $fh "# (the generated/ set next to this file); both are set by CMakeLists.txt before include().\n\n";
print $fh "set(OSSL_CRYPTO_SRCS\n";
emit($fh, $_) for @crypto;
print $fh ")\n\n";
print $fh "set(OSSL_SSL_SRCS\n";
emit($fh, $_) for @ssl;
print $fh ")\n";
close($fh);

printf "exported %d generated files, %d libcrypto sources, %d libssl sources\n",
    $count, scalar(@crypto), scalar(@ssl);
