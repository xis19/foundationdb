set(TUTORIAL_SRCS tutorial.actor.cpp)
set(EXPERIMENT_SOURCES experiment.actor.cpp)

add_flow_target(EXECUTABLE NAME tutorial SRCS "${TUTORIAL_SRCS}")
target_link_libraries(tutorial PUBLIC fdbclient)

add_flow_target(EXECUTABLE NAME experiment SRCS "${EXPERIMENT_SOURCES}")
target_link_libraries(experiment PUBLIC fdbclient)
