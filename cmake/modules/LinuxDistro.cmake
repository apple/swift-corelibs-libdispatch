function (get_distro_name valuename INTO distroname)
  file (STRINGS /etc/os-release distro REGEX "^${valuename}=")
  string (REGEX REPLACE "^${valuename}=\"?\(.*\)" "\\1" ${distroname} "${distro}")
  string (REGEX REPLACE "\"$" "" ${distroname} "${${distroname}}")
  set (${distroname} "${${distroname}}" PARENT_SCOPE)
endfunction (get_distro_name valuename INTO distroname)

