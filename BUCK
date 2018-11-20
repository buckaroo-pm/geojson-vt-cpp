load('//:buckaroo_macros.bzl', 'buckaroo_deps')

prebuilt_cxx_library(
  name = 'geojson-vt-cpp', 
  header_namespace = 'mapbox', 
  header_only = True, 
  exported_headers = subdir_glob([
    ('include/mapbox', '**/*.hpp'), 
  ]), 
  deps = buckaroo_deps(), 
  visibility = [
    'PUBLIC', 
  ], 
)
