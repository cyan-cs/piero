__declspec(dllimport) unsigned wf_asan_pool_read(unsigned index);

int main(int argc, char **argv)
{
	(void)argv;
	return (int)wf_asan_pool_read(argc > 1 ? 1u : 0u);
}
